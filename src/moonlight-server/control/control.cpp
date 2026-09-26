#include "core/input.hpp"
#include <algorithm>
#include <array>
#include <boost/endian/conversion.hpp>
#include <cmath>
#include <control/control.hpp>
#include <control/input_handler.hpp>
#include <cstdlib>
#include <deque>
#include <events/events.hpp>
#include <immer/box.hpp>
#include <mutex>
#include <state/sessions.hpp>
#include <string>
#include <sys/socket.h>
#include <unordered_map>

namespace control {

using namespace ranges;
using namespace moonlight::control;
using namespace wolf::core::events;

namespace {

struct PendingPacket {
  std::string payload;
  std::shared_ptr<ENetPeer> peer;
};

struct PendingHapticAudio {
  std::string session_id;
  std::array<std::int8_t, 64> pcm;
};

std::mutex pending_packets_mutex;
std::deque<PendingPacket> pending_packets;
std::deque<PendingHapticAudio> pending_haptic_audio;
std::unordered_map<ENetPeer *, std::uint32_t> next_packet_sequences;
// Moonlight retains its audio-haptic replay guard when the encrypted ENet
// channel reconnects. Key this sequence by the stable stream session rather
// than the short-lived ENetPeer, otherwise every reconnect restarts at zero
// and the client correctly discards subsequent haptic PCM as stale.
std::unordered_map<std::string, std::uint16_t> next_haptic_audio_sequences;
std::uint64_t haptic_audio_packets_sent = 0;
std::uint64_t haptic_audio_packets_dropped = 0;

struct HapticTestProbe {
  std::string aes_key;
  std::chrono::steady_clock::time_point next_packet;
  std::uint16_t sequence = 0;
  std::uint32_t sample_offset = 0;
  std::uint16_t packets_in_stage = 0;
  std::uint8_t stage = 0; // left-only, silence, right-only
};

std::unordered_map<ENetPeer *, HapticTestProbe> haptic_test_probes;

void queue_haptic_test_probe_packets() {
  using namespace std::chrono_literals;
  constexpr auto packet_interval = std::chrono::microseconds{10667};
  constexpr std::uint16_t left_or_right_packets = 94;
  constexpr std::uint16_t silence_packets = 30;

  const auto now = std::chrono::steady_clock::now();
  for (auto it = haptic_test_probes.begin(); it != haptic_test_probes.end();) {
    auto *peer = it->first;
    auto &probe = it->second;
    if (!peer || peer->state != ENET_PEER_STATE_CONNECTED) {
      it = haptic_test_probes.erase(it);
      continue;
    }
    if (now < probe.next_packet) {
      ++it;
      continue;
    }

    moonlight::control::ControlDualSenseHapticAudioPacket packet{};
    packet.header = {.type = moonlight::control::pkts::DUALSENSE_HAPTIC_AUDIO,
                     .length = boost::endian::native_to_little(static_cast<std::uint16_t>(72))};
    packet.version = 1;
    packet.controller_id = 0;
    packet.sequence = boost::endian::native_to_little(probe.sequence++);
    packet.sample_rate = boost::endian::native_to_little(static_cast<std::uint16_t>(3000));
    packet.frame_count = boost::endian::native_to_little(static_cast<std::uint16_t>(32));

    for (std::size_t frame = 0; frame < 32; ++frame) {
      // One 64-sample cycle at 3 kHz produces the 46.875 Hz carrier used by
      // the receiver's DualSense output-report mapper.
      const auto phase = (probe.sample_offset++ % 64) * (2.0 * 3.14159265358979323846 / 64.0);
      const auto sample = static_cast<std::int8_t>(std::lround(std::sin(phase) * 96.0));
      const bool active = probe.stage != 1;
      const bool left = probe.stage == 0;
      packet.pcm[frame * 2] = active && left ? sample : 0;
      packet.pcm[frame * 2 + 1] = active && !left ? sample : 0;
    }

    const std::string_view plaintext{reinterpret_cast<const char *>(&packet), sizeof(packet)};
    const auto queued =
        encrypt_and_send(plaintext, probe.aes_key, immer::box<std::shared_ptr<ENetPeer>>{to_shared_ptr(peer)});
    if (!queued) {
      logs::log(logs::warning, "[SCEPAD_AUDIO_TEST] failed to queue seq={} stage={}", probe.sequence - 1, probe.stage);
    }

    ++probe.packets_in_stage;
    const auto stage_limit = probe.stage == 1 ? silence_packets : left_or_right_packets;
    if (probe.packets_in_stage >= stage_limit) {
      probe.packets_in_stage = 0;
      ++probe.stage;
      if (probe.stage >= 3) {
        logs::log(logs::info, "[SCEPAD_AUDIO_TEST] left/silence/right probe completed");
        it = haptic_test_probes.erase(it);
        continue;
      }
      logs::log(logs::info, "[SCEPAD_AUDIO_TEST] starting {} channel probe", probe.stage == 1 ? "silence" : "right");
    }
    probe.next_packet = now + packet_interval;
    ++it;
  }
}

} // namespace

void free_host(ENetHost *host) {
  std::for_each(host->peers, host->peers + host->peerCount, [](ENetPeer &peer_ref) {
    ENetPeer *peer = &peer_ref;

    if (peer) {
      enet_peer_disconnect_now(peer, 0);
    }
  });

  enet_host_destroy(host);
}

using enet_host = std::unique_ptr<ENetHost, decltype(&free_host)>;
using enet_packet = std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)>;

bool init() {
  auto error_code = enet_initialize();
  if (error_code != 0) {
    logs::log(logs::error, "An error occurred while initializing Enet: {}.", error_code);
    return false;
  }
  return true;
}

enet_host create_host(std::string_view host, std::uint16_t port, std::size_t peers) {
  ENetAddress addr;
  enet_address_set_host(&addr, host.data());
  enet_address_set_port(&addr, port);

  auto enet_host = enet_host_create(AF_INET, &addr, peers, 0, 0, 0);
  if (enet_host == nullptr) {
    logs::log(logs::error, "An error occurred while trying to create an ENet server host.");
  }

  return {enet_host, free_host};
}

/**
 * The Moonlight fork of ENET doesn't include host and port as easily accessible parts of the struct,
 * we have to extract them manually
 */
std::pair<std::string /* ip */, int /* port */> get_ip(const sockaddr *const ip_addr) {
  char data[INET6_ADDRSTRLEN];

  auto family = ip_addr->sa_family;
  std::uint16_t port;
  if (family == AF_INET6) {
    inet_ntop(AF_INET6, &((sockaddr_in6 *)ip_addr)->sin6_addr, data, INET6_ADDRSTRLEN);
    port = ((sockaddr_in6 *)ip_addr)->sin6_port;
  }

  if (family == AF_INET) {
    inet_ntop(AF_INET, &((sockaddr_in *)ip_addr)->sin_addr, data, INET_ADDRSTRLEN);
    port = ((sockaddr_in *)ip_addr)->sin_port;
  }

  return {std::string{data}, port};
}

bool send_packet(std::string_view payload, ENetPeer *peer) {
  logs::log(logs::trace, "[ENET] Sending packet");
  auto packet = enet_packet_create(payload.data(), payload.size(), ENET_PACKET_FLAG_RELIABLE);
  if (enet_peer_send(peer, 0, packet) < 0) {
    logs::log(logs::warning, "[ENET] Failed to send packet");
    enet_packet_destroy(packet);
    return false;
  }
  return true;
}

namespace {

void send_pending_packets() {
  std::deque<PendingPacket> batch;
  {
    std::lock_guard lock(pending_packets_mutex);
    batch.swap(pending_packets);
  }

  for (const auto &packet : batch) {
    send_packet(packet.payload, packet.peer.get());
  }
}

// ENetPeer objects identify one transport connection, not the Moonlight
// session.  A reconnect deliberately keeps the session (and its virtual
// DualSense) alive, but packets encrypted for the old peer can never be
// delivered after its disconnect.  Drop only those stale packets; haptic
// audio is session-keyed and is rebuilt for the new peer on resume.
void discard_pending_packets_for_peer(ENetPeer *peer) {
  std::lock_guard lock(pending_packets_mutex);
  std::erase_if(pending_packets, [peer](const PendingPacket &packet) { return packet.peer.get() == peer; });
  next_packet_sequences.erase(peer);
}

void send_pending_haptic_audio(const enet_clients_map &connected_clients) {
  std::deque<PendingHapticAudio> batch;
  {
    std::lock_guard lock(pending_packets_mutex);
    batch.swap(pending_haptic_audio);
  }

  for (const auto &frame : batch) {
    ENetPeer *target_peer = nullptr;
    const events::StreamSession *target_session = nullptr;
    ENetPeer *sole_capable_peer = nullptr;
    const events::StreamSession *sole_capable_session = nullptr;
    size_t capable_clients = 0;

    // The Pulse callback receives the session selected by the KDE lobby
    // router.  Normally it exactly matches the control connection.  During
    // a client reconnect, however, the audio thread and control thread can
    // observe the new connection on adjacent ticks.  Keep the strict match
    // first, but make that short hand-over robust when this is the only
    // haptic-capable client.  We never use this fallback with two clients.
    for (const auto &[peer, session] : connected_clients) {
      if (!peer)
        continue;
      if (!(session->client_feature_flags->load(std::memory_order_acquire) & ML_FF_DUALSENSE_HAPTIC_AUDIO))
        continue;

      ++capable_clients;
      sole_capable_peer = peer;
      sole_capable_session = &session.get();
      if (std::to_string(session->session_id) == frame.session_id) {
        target_peer = peer;
        target_session = &session.get();
        break;
      }
    }

    if (!target_peer && capable_clients == 1) {
      target_peer = sole_capable_peer;
      target_session = sole_capable_session;
      if ((haptic_audio_packets_sent % 64) == 0) {
        logs::log(logs::warning,
                  "[SCEPAD_AUDIO] session '{}' was not yet present in the control map; "
                  "using its sole haptic-capable client '{}',",
                  frame.session_id,
                  target_session->session_id);
      }
    }

    if (!target_peer || !target_session) {
      if (++haptic_audio_packets_dropped % 64 == 1) {
        logs::log(logs::warning,
                  "[SCEPAD_AUDIO] dropped haptic frame for session '{}': {} capable control client(s)",
                  frame.session_id,
                  capable_clients);
      }
      continue;
    }

    moonlight::control::ControlDualSenseHapticAudioPacket packet{};
    packet.header = {.type = moonlight::control::pkts::DUALSENSE_HAPTIC_AUDIO,
                     .length = boost::endian::native_to_little(static_cast<std::uint16_t>(72))};
    packet.version = 1;
    packet.controller_id = 0;
    const auto haptic_session_id = std::to_string(target_session->session_id);
    packet.sequence = boost::endian::native_to_little(next_haptic_audio_sequences[haptic_session_id]++);
    packet.sample_rate = boost::endian::native_to_little(static_cast<std::uint16_t>(3000));
    packet.frame_count = boost::endian::native_to_little(static_cast<std::uint16_t>(32));
    packet.pcm = frame.pcm;
    const std::string_view plaintext{reinterpret_cast<const char *>(&packet), sizeof(packet)};
    if (!encrypt_and_send(plaintext,
                          target_session->aes_key,
                          immer::box<std::shared_ptr<ENetPeer>>{to_shared_ptr(target_peer)})) {
      logs::log(logs::warning, "[SCEPAD_AUDIO] failed to queue haptic packet for session {}", frame.session_id);
    } else if (++haptic_audio_packets_sent % 64 == 1) {
      logs::log(logs::debug,
                "[SCEPAD_AUDIO] encrypted and queued haptic packet {} for session {}",
                haptic_audio_packets_sent,
                frame.session_id);
    }
  }
}

} // namespace

bool encrypt_and_send(std::string_view payload,
                      std::string_view aes_key,
                      immer::box<std::shared_ptr<ENetPeer>> connected_client) {
  if (auto enet_client = *connected_client) {
    {
      std::lock_guard lock(pending_packets_mutex);
      auto &sequence = next_packet_sequences[enet_client.get()];
      auto encrypted = control::encrypt_packet(aes_key, sequence++, payload);
      std::string packet{(char *)encrypted.get(), encrypted->full_size()};
      pending_packets.push_back({std::move(packet), std::move(enet_client)});
    }
    return true;
  } else {
    logs::log(logs::warning, "[ENET] Failed to send packet, client is not connected");
    return false;
  }
}

void queue_dualsense_haptic_audio(std::string session_id, std::array<std::int8_t, 64> pcm) {
  if (session_id.empty())
    return;
  std::lock_guard lock(pending_packets_mutex);
  // Keep Pulse's real-time callback bounded while no client is available.
  if (pending_haptic_audio.size() >= 256)
    pending_haptic_audio.pop_front();
  pending_haptic_audio.push_back({std::move(session_id), pcm});
}

bool send_hdr_mode(const events::StreamSession &session, immer::box<std::shared_ptr<ENetPeer>> client, bool enabled) {
  auto packet = moonlight::control::hdr_mode_packet(enabled);
  std::string_view plaintext{reinterpret_cast<const char *>(&packet), sizeof(packet)};
  logs::log(logs::debug, "[ENET] Sending HDR mode {} to session {}", enabled, session.session_id);
  return encrypt_and_send(plaintext, session.aes_key, client);
}

std::optional<events::StreamSession> get_current_session(const enet_clients_map &connected_clients,
                                                         const state::SessionsAtoms &running_sessions,
                                                         std::string_view client_ip,
                                                         const ENetEvent &enet_event) {
  if (enet_event.type == ENET_EVENT_TYPE_CONNECT) {
    // A new connection, we should check if there's a session that matches the current client
    for (const StreamSession &session : *running_sessions->load()) {
      if (session.enet_secret_payload == enet_event.data) {
        return session;
      }
    }
    logs::log(logs::warning,
              "[ENET] Unable to find a session that matches the client secret {}, matching by IP",
              enet_event.data);
    for (const StreamSession &session : *running_sessions->load()) {
      if (session.ip == client_ip) {
        return session;
      }
    }
  } else {
    // The connection has already been established, we'll check for a match in our connected client map
    if (auto client = connected_clients.find(enet_event.peer)) {
      return client->get();
    }
  }
  return std::nullopt;
}

std::shared_ptr<ENetPeer> to_shared_ptr(ENetPeer *peer) {
  return std::shared_ptr<ENetPeer>(peer, [](auto _peer) {
    // DO NOTHING, we don't want to free peer, the lifecycle is dictated by enet
  });
}

void run_control(int port,
                 const state::SessionsAtoms &running_sessions,
                 const std::shared_ptr<events::EventBusType> &event_bus,
                 int peers,
                 std::chrono::milliseconds timeout,
                 const std::string &host_ip) {

  enet_host host = create_host(host_ip, port, peers);
  logs::log(logs::info, "Control server started on port: {}", port);
  const auto haptic_test_env = std::getenv("WOLF_SCEPAD_HAPTIC_TEST");
  const bool haptic_test_enabled = haptic_test_env && std::string_view{haptic_test_env} == "1";
  if (haptic_test_enabled) {
    logs::log(logs::warning,
              "[SCEPAD_AUDIO_TEST] enabled; newly connected capable clients will receive a left/silence/right probe");
  }

  ENetEvent event;

  immer::atom<enet_clients_map> connected_clients;
  immer::atom<immer::map<std::size_t, bool>> hdr_modes;

  auto stop_ev = event_bus->register_handler<immer::box<StopStreamEvent>>(
      [&connected_clients, &hdr_modes](const immer::box<StopStreamEvent> &ev) {
        hdr_modes.update([ev](const immer::map<std::size_t, bool> &m) { return m.erase(ev->session_id); });
        auto terminate_pkt = ControlTerminatePacket{};
        std::string plaintext = {(char *)&terminate_pkt, sizeof(terminate_pkt)};
        for (auto &[peer, session] : *connected_clients.load()) {
          if (session->session_id == ev->session_id) {
            immer::box<std::shared_ptr<ENetPeer>> enet_client = {to_shared_ptr(peer)};
            encrypt_and_send(plaintext, session->aes_key, enet_client);
            return;
          }
        }
        logs::log(logs::debug, "[ENET] Client not found for session: {}", ev->session_id);
      });

  auto video_ev = event_bus->register_handler<immer::box<VideoSession>>(
      [&connected_clients, &hdr_modes](const immer::box<VideoSession> &video) {
        hdr_modes.update(
            [video](const immer::map<std::size_t, bool> &m) { return m.set(video->session_id, video->hdr_requested); });

        for (auto &[peer, session] : *connected_clients.load()) {
          if (session->session_id == video->session_id) {
            send_hdr_mode(*session, immer::box<std::shared_ptr<ENetPeer>>{to_shared_ptr(peer)}, video->hdr_requested);
            return;
          }
        }
      });

  while (true) {
    const auto service_timeout = std::min(timeout, 10ms);
    const auto service_result = enet_host_service(host.get(), &event, service_timeout.count());
    queue_haptic_test_probe_packets();
    send_pending_haptic_audio(*connected_clients.load());
    send_pending_packets();

    if (service_result > 0) {
      auto [client_ip, client_port] = get_ip((sockaddr *)&event.peer->address.address);
      auto client_session = get_current_session(connected_clients, running_sessions, client_ip, event);
      if (client_session) {
        switch (event.type) {
        case ENET_EVENT_TYPE_NONE:
          break;
        case ENET_EVENT_TYPE_CONNECT:
          logs::log(logs::debug, "[ENET] connected client: {}:{}", client_ip, client_port);
          {
            std::lock_guard lock(pending_packets_mutex);
            next_packet_sequences.erase(event.peer);
          }
          connected_clients.update([peer = event.peer, client_session](const enet_clients_map &m) {
            return m.set(peer, client_session.value());
          });
          if (haptic_test_enabled) {
            const auto flags = client_session->client_feature_flags->load(std::memory_order_acquire);
            if (flags & ML_FF_DUALSENSE_HAPTIC_AUDIO) {
              haptic_test_probes.insert_or_assign(
                  event.peer,
                  HapticTestProbe{.aes_key = client_session->aes_key,
                                  .next_packet = std::chrono::steady_clock::now() + 500ms});
              logs::log(logs::info,
                        "[SCEPAD_AUDIO_TEST] negotiated with client {}; queuing left/silence/right probe",
                        client_ip);
            } else {
              logs::log(logs::warning,
                        "[SCEPAD_AUDIO_TEST] client {} did not negotiate capability bit 0x04 (flags=0x{:02x})",
                        client_ip,
                        flags);
            }
          }
          event_bus->fire_event(
              immer::box<ResumeStreamEvent>(ResumeStreamEvent{.session_id = client_session->session_id}));
          if (auto hdr = hdr_modes.load()->find(client_session->session_id)) {
            send_hdr_mode(client_session.value(),
                          immer::box<std::shared_ptr<ENetPeer>>{to_shared_ptr(event.peer)},
                          *hdr);
          }
          break;
        case ENET_EVENT_TYPE_DISCONNECT:
          logs::log(logs::debug, "[ENET] disconnected client: {}:{}", client_ip, client_port);
          discard_pending_packets_for_peer(event.peer);
          haptic_test_probes.erase(event.peer);
          connected_clients.update([peer = event.peer](const enet_clients_map &m) { return m.erase(peer); });
          // An ENet disconnect is a transport interruption, not an explicit
          // request to leave the running app.  Leaving the lobby here sends
          // udev remove events into the game sandbox.  ScePad/Wwise games
          // generally retain their four-channel haptic endpoint for the
          // life of the process, so removing and re-adding the virtual
          // DualSense makes feedback stop after Moonlight reconnects.
          //
          // Keep the lobby and device identity alive.  The ResumeStream
          // path will rebind the persistent controller and haptic stream to
          // the new ENet peer.  Explicit TERMINATION packets still emit
          // PauseStreamEvent and retain the normal exit-to-Wolf-UI behavior.
          logs::log(logs::info,
                    "[ENET] preserving session {} and its virtual devices across client reconnect",
                    client_session->session_id);
          break;
        case ENET_EVENT_TYPE_RECEIVE:
          enet_packet packet = {event.packet, enet_packet_destroy};

          auto type = ((ControlPacket *)packet->data)->type;

          logs::log(logs::trace,
                    "[ENET] received {} of {} bytes from: {}:{} HEX: {}",
                    packet_type_to_str(type),
                    packet->dataLength,
                    client_ip,
                    client_port,
                    crypto::str_to_hex({(char *)packet->data, packet->dataLength}));

          if (type == ENCRYPTED) {
            try {
              auto enc_pkt = (ControlEncryptedPacket *)(packet->data);
              auto decrypted = decrypt_packet(*enc_pkt, client_session->aes_key);
              auto sub_type = ((ControlPacket *)decrypted.data())->type;

              logs::log(logs::trace,
                        "[ENET] decrypted sub_type: {} HEX: {}",
                        packet_type_to_str(sub_type),
                        crypto::str_to_hex(decrypted));

              if (sub_type == TERMINATION) {
                event_bus->fire_event(
                    immer::box<PauseStreamEvent>(PauseStreamEvent{.session_id = client_session->session_id}));
              } else if (sub_type == INPUT_DATA) {
                immer::box<std::shared_ptr<ENetPeer>> enet_client = {to_shared_ptr(event.peer)};
                handle_input(client_session.value(), enet_client, (INPUT_PKT *)decrypted.data());
              } else if (sub_type == IDR_FRAME) {
                auto ev = IDRRequestEvent{.session_id = client_session->session_id};
                event_bus->fire_event(immer::box<IDRRequestEvent>{ev});
              }
            } catch (std::runtime_error &e) {
              logs::log(logs::warning, "[ENET] Unable to decrypt incoming packet: {}", e.what());
            }
          } else {
            logs::log(logs::warning,
                      "[ENET] Received unencrypted message: {} - {}",
                      packet_type_to_str(type),
                      crypto::str_to_hex({(char *)packet->data, packet->dataLength}));
          }
          break;
        }
      } else {
        logs::log(logs::warning, "[ENET] Received packet from unrecognised client {}:{}", client_ip, client_port);
        enet_peer_disconnect_now(event.peer, 0);
      }
    }
  }

  stop_ev.unregister();
  video_ev.unregister();
}

} // namespace control
