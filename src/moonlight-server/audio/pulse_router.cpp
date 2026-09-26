#include "pulse_router.hpp"
#include <immer/vector_transient.hpp>

#include <control/control.hpp>
#include <core/audio.hpp>
#include <helpers/logger.hpp>

#include <pulse/pulseaudio.h>
#include <sessions/common.hpp>
#include <state/sessions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

using wolf::core::sessions::VIRTUAL_SINK_PREFIX;

namespace {
constexpr std::string_view SCEPAD_SINK_NAME{"control_dualsense_audio"};
constexpr unsigned SCEPAD_INPUT_RATE = 48000;
constexpr unsigned SCEPAD_OUTPUT_RATE = 3000;
constexpr size_t SCEPAD_DECIMATION = SCEPAD_INPUT_RATE / SCEPAD_OUTPUT_RATE;
constexpr size_t SCEPAD_PACKET_FRAMES = 32;

std::int8_t float_to_s8(float sample) {
  const auto scaled = std::lround(std::clamp(sample, -1.0f, 1.0f) * 127.0f);
  return static_cast<std::int8_t>(std::clamp<long>(scaled, -127, 127));
}
} // namespace

namespace wolf::core::audio {

immer::vector<immer::box<events::EventBusHandlers>>
setup_pulseaudio_router_handlers(const immer::box<state::AppState> &app_state,
                                 std::shared_ptr<PulseAudioRouterState> state) {
  immer::vector_transient<immer::box<events::EventBusHandlers>> handlers;

  if (!state) {
    logs::log(logs::warning, "[PULSE_ROUTER] No router state provided, cannot setup handlers");
    return handlers.persistent();
  }
  if (!app_state->event_bus) {
    logs::log(logs::warning, "[PULSE_ROUTER] No event bus provided, cannot setup handlers");
    return handlers.persistent();
  }
  if (!state->pulse_server) {
    logs::log(logs::warning, "[PULSE_ROUTER] No Pulse server provided, cannot setup handlers");
    return handlers.persistent();
  }

  // Register handlers (Wolf pattern: store registrations in `handlers`)
  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::DockerContainerCreated>>(
      [state](const immer::box<events::DockerContainerCreated> &ev) {
        if (state)
          state->on_container_created(*ev);
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::DockerContainerStopped>>(
      [state](const immer::box<events::DockerContainerStopped> &ev) {
        if (state)
          state->on_container_stopped(*ev);
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::JoinLobbyEvent>>(
      [state](const immer::box<events::JoinLobbyEvent> &ev) {
        if (state)
          state->on_lobby_joined(*ev);
      }));

  handlers.push_back(app_state->event_bus->register_handler<immer::box<events::LeaveLobbyEvent>>(
      [state](const immer::box<events::LeaveLobbyEvent> &ev) {
        if (state)
          state->on_lobby_left(*ev);
      }));

  // Subscribe to sink-input events and do an initial scan
  state->enable_pulse_subscribe();

  logs::log(logs::info, "[PULSE_ROUTER] Setup complete (handlers registered, PA subscribed)");
  return handlers.persistent();
}

// --- PulseAudioRouterState implementation ------------------------------------

void PulseAudioRouterState::rescan() {
  if (!pulse_server)
    return;

  audio::queue_op(pulse_server, [this]() {
    if (!pulse_server)
      return;
    auto *c = audio::context(pulse_server);
    auto op = pa_context_get_sink_input_info_list(c, &PulseAudioRouterState::pa_sink_input_info_cb, this);
    if (op)
      pa_operation_unref(op);
  });
}

void PulseAudioRouterState::on_container_created(const events::DockerContainerCreated &ev) {
  if (ev.hostname.empty() || ev.session_id.empty())
    return;

  host_to_session.update([&](auto m) { return m.set(ev.hostname, ev.session_id); });

  logs::log(logs::debug,
            "[PULSE_ROUTER] Map add host='{}' -> session='{}' (container_id='{}')",
            ev.hostname,
            ev.session_id,
            ev.container_id);

  // If sink-input already exists, route it now.
  rescan();
}

void PulseAudioRouterState::on_container_stopped(const events::DockerContainerStopped &ev) {
  if (ev.hostname.empty())
    return;

  std::optional<std::string> removed_session;

  host_to_session.update([&](auto m) {
    if (auto v = m.find(ev.hostname)) { // hostname -> session_id
      // optionally guard against hostname reuse:
      if (ev.session_id.empty() || *v == ev.session_id) {
        removed_session = *v;
        return m.erase(ev.hostname);
      }
    }
    return m;
  });

  if (removed_session && !removed_session->empty()) {
    session_to_sink_idx.update([&](auto m) { return m.erase(*removed_session); });

    logs::log(logs::debug, "[PULSE_ROUTER] Removed mappings host='{}' session='{}'", ev.hostname, *removed_session);
  }
}

void PulseAudioRouterState::on_lobby_joined(const events::JoinLobbyEvent &ev) {
  if (ev.lobby_id.empty())
    return;

  const auto session_id = std::to_string(ev.moonlight_session_id);
  lobby_to_moonlight_session.update([&](auto m) { return m.set(ev.lobby_id, session_id); });
  logs::log(logs::debug, "[SCEPAD_AUDIO] Map lobby '{}' to Moonlight session '{}'", ev.lobby_id, session_id);

  // A Moonlight reconnect leaves the game and its quad Wwise stream alive,
  // but recreates the numeric control session.  Pulse only reports a sink
  // input when it is created or changed, so do not wait for another report:
  // rebind the already-associated lobby stream now.
  if (scepad_lobby_id == ev.lobby_id) {
    scepad_session_id = session_id;
    scepad_pcm.clear();
    scepad_haptics_active = false;
    logs::log(logs::info,
              "[SCEPAD_AUDIO] Rebound existing quad stream for lobby '{}' to reconnected Moonlight session '{}'",
              ev.lobby_id,
              session_id);
  }
}

void PulseAudioRouterState::on_lobby_left(const events::LeaveLobbyEvent &ev) {
  if (ev.lobby_id.empty())
    return;

  const auto session_id = std::to_string(ev.moonlight_session_id);
  lobby_to_moonlight_session.update([&](auto m) {
    if (auto mapped_session = m.find(ev.lobby_id); mapped_session && *mapped_session == session_id)
      return m.erase(ev.lobby_id);
    return m;
  });

  if (scepad_session_id == session_id || scepad_lobby_id == ev.lobby_id) {
    scepad_session_id.clear();
    scepad_pcm.clear();
    scepad_haptics_active = false;
  }
  logs::log(logs::debug,
            "[SCEPAD_AUDIO] Remove lobby '{}' mapping for Moonlight session '{}'",
            ev.lobby_id,
            session_id);
}

// resolve sink name to sink index
void PulseAudioRouterState::pa_sink_info_cb(pa_context *c, const pa_sink_info *info, int eol, void *userdata) {
  if (eol != 0 || !info)
    return;
  auto *self = static_cast<PulseAudioRouterState *>(userdata);
  if (!self)
    return;

  if (!info->name)
    return;
  std::string_view name{info->name};

  if (name == SCEPAD_SINK_NAME) {
    self->scepad_sink_idx = info->index;
    self->start_scepad_monitor_(c);
    logs::log(logs::info, "[SCEPAD_AUDIO] Found quad haptic sink '{}' idx={}", info->name, info->index);
    return;
  }

  // Prefix match
  if (!name.starts_with(VIRTUAL_SINK_PREFIX))
    return;

  // session_id is suffix after prefix
  std::string session_id{name.substr(VIRTUAL_SINK_PREFIX.size())};

  self->session_to_sink_idx.update([&](auto m) { return m.set(session_id, info->index); });

  logs::log(logs::debug,
            "[PULSE_ROUTER] Sink appeared name='{}' idx={} -> session='{}'",
            info->name,
            info->index,
            session_id);

  // route anything pending
  self->rescan();
}

void PulseAudioRouterState::enable_pulse_subscribe() {
  if (!pulse_server)
    return;

  audio::queue_op(pulse_server, [this]() {
    if (!pulse_server)
      return;
    auto *c = audio::context(pulse_server);

    pa_context_set_subscribe_callback(c, &PulseAudioRouterState::pa_subscribe_cb, this);

    auto op = pa_context_subscribe(
        c,
        static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SINK),
        nullptr,
        nullptr);
    if (op)
      pa_operation_unref(op);

    // Initial scan
    auto op2 = pa_context_get_sink_input_info_list(c, &PulseAudioRouterState::pa_sink_input_info_cb, this);
    if (op2)
      pa_operation_unref(op2);

    auto op3 = pa_context_get_sink_info_list(c, &PulseAudioRouterState::pa_sink_info_cb, this);
    if (op3)
      pa_operation_unref(op3);

    logs::log(logs::debug, "[PULSE_ROUTER] Enabled PulseAudio subscription (sink-input + sink)");
  });
}

void PulseAudioRouterState::pa_subscribe_cb(pa_context *c,
                                            pa_subscription_event_type_t t,
                                            uint32_t idx,
                                            void *userdata) {
  auto *self = static_cast<PulseAudioRouterState *>(userdata);
  if (!self)
    return;

  const auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  const auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

  if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
    if (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_CHANGE) {
      auto op = pa_context_get_sink_input_info(c, idx, &PulseAudioRouterState::pa_sink_input_info_cb, userdata);
      if (op)
        pa_operation_unref(op);
    }
    return;
  }

  if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
    if (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_CHANGE) {
      auto op = pa_context_get_sink_info_by_index(c, idx, &PulseAudioRouterState::pa_sink_info_cb, userdata);
      if (op)
        pa_operation_unref(op);
    }
    return;
  }
}

void PulseAudioRouterState::pa_sink_input_info_cb(pa_context *c,
                                                  const pa_sink_input_info *info,
                                                  int eol,
                                                  void *userdata) {
  if (eol || !info)
    return;
  auto *self = static_cast<PulseAudioRouterState *>(userdata);
  if (!self)
    return;

  self->route_sink_input_(c, info);
}

void PulseAudioRouterState::route_sink_input_(pa_context *c, const pa_sink_input_info *info) {
  if (!info || !info->proplist)
    return;

  const char *host = pa_proplist_gets(info->proplist, "application.process.host");
  if (!host || host[0] == '\0')
    return;

  // host -> session_id
  std::optional<std::string> session_id;
  {
    auto box = host_to_session.load();
    const auto &m = *box;
    if (auto v = m.find(std::string(host))) {
      session_id = *v; // immer::map find returns value ptr
    }
  }
  if (!session_id || session_id->empty())
    return;

  // The controller's own quad endpoint must stay separate from the desktop
  // stream.  A game runner is keyed by a lobby UUID, so resolve it to the
  // numeric session used by the Moonlight control channel before forwarding.
  if (info->sink == scepad_sink_idx) {
    std::string output_session_id = *session_id;
    scepad_lobby_id = output_session_id;
    {
      auto lobby_sessions = lobby_to_moonlight_session.load();
      if (auto mapped_session = lobby_sessions->find(output_session_id))
        output_session_id = *mapped_session;
    }
    scepad_session_id = std::move(output_session_id);
    logs::log(logs::debug,
              "[SCEPAD_AUDIO] Associate quad stream {} host='{}' with Moonlight session '{}'",
              info->index,
              host,
              scepad_session_id);
    return;
  }

  // session_id -> sink index
  std::optional<uint32_t> target_sink_idx;
  {
    auto box = session_to_sink_idx.load();
    const auto &m = *box;
    if (auto v = m.find(*session_id)) {
      target_sink_idx = *v;
    }
  }
  if (!target_sink_idx)
    return;

  // Already routed?
  if (info->sink == *target_sink_idx)
    return;

  // Move by index (robust)
  auto op = pa_context_move_sink_input_by_index(c, info->index, *target_sink_idx, nullptr, nullptr);
  if (op)
    pa_operation_unref(op);

  logs::log(logs::debug,
            "[PULSE_ROUTER] Move sink-input={} host='{}' -> session='{}' sink_idx={}",
            info->index,
            host,
            *session_id,
            *target_sink_idx);
}

void PulseAudioRouterState::start_scepad_monitor_(pa_context *c) {
  if (scepad_monitor)
    return;

  pa_sample_spec spec{};
  spec.format = PA_SAMPLE_FLOAT32LE;
  spec.rate = SCEPAD_INPUT_RATE;
  spec.channels = 4;
  if (!pa_sample_spec_valid(&spec)) {
    logs::log(logs::error, "[SCEPAD_AUDIO] invalid monitor sample specification");
    return;
  }
  pa_channel_map map{};
  pa_channel_map_init(&map);
  map.channels = spec.channels;
  map.map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
  map.map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
  map.map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
  map.map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
  auto *stream = pa_stream_new(c, "Wolf DualSense ScePad haptic forwarder", &spec, &map);
  if (!stream) {
    logs::log(logs::error, "[SCEPAD_AUDIO] could not create Pulse monitor stream");
    return;
  }
  pa_stream_set_read_callback(stream, &PulseAudioRouterState::pa_scepad_read_cb, this);
  pa_buffer_attr attr{};
  attr.maxlength = static_cast<uint32_t>(-1);
  attr.fragsize = 16 * 4 * SCEPAD_PACKET_FRAMES;
  if (pa_stream_connect_record(stream,
                               "control_dualsense_audio.monitor",
                               &attr,
                               static_cast<pa_stream_flags_t>(PA_STREAM_ADJUST_LATENCY)) < 0) {
    logs::log(logs::error, "[SCEPAD_AUDIO] cannot monitor quad sink: {}", pa_strerror(pa_context_errno(c)));
    pa_stream_unref(stream);
    return;
  }
  scepad_monitor = stream;
  logs::log(logs::info, "[SCEPAD_AUDIO] monitoring rear channels from control_dualsense_audio.monitor");
}

void PulseAudioRouterState::pa_scepad_read_cb(pa_stream *stream, size_t /*length*/, void *userdata) {
  auto *self = static_cast<PulseAudioRouterState *>(userdata);
  if (!self)
    return;
  const void *data = nullptr;
  size_t bytes = 0;
  while (pa_stream_peek(stream, &data, &bytes) == 0 && bytes > 0) {
    if (data)
      self->consume_scepad_pcm_(static_cast<const float *>(data), bytes / (sizeof(float) * 4));
    pa_stream_drop(stream);
    data = nullptr;
    bytes = 0;
  }
}

void PulseAudioRouterState::consume_scepad_pcm_(const float *samples, size_t frames) {
  if (scepad_session_id.empty() || !samples)
    return;
  scepad_pcm.insert(scepad_pcm.end(), samples, samples + frames * 4);

  constexpr size_t input_frames_per_packet = SCEPAD_PACKET_FRAMES * SCEPAD_DECIMATION;
  while (scepad_pcm.size() >= input_frames_per_packet * 4) {
    std::array<std::int8_t, 64> packet{};
    for (size_t frame = 0; frame < SCEPAD_PACKET_FRAMES; ++frame) {
      const auto input_frame = frame * SCEPAD_DECIMATION;
      // USB DualSense: FL/FR are its speaker; RL/RR are the two haptic coils.
      packet[frame * 2] = float_to_s8(scepad_pcm[input_frame * 4 + 2]);
      packet[frame * 2 + 1] = float_to_s8(scepad_pcm[input_frame * 4 + 3]);
    }
    const bool active = std::any_of(packet.begin(), packet.end(), [](std::int8_t sample) { return sample != 0; });
    // The null sink continuously supplies silence.  Forward nonzero haptic
    // PCM and exactly one following silent frame, rather than generating
    // 93 encrypted no-op control packets per second while a game is idle.
    if (active || scepad_haptics_active) {
      control::queue_dualsense_haptic_audio(scepad_session_id, packet);
      ++scepad_packets_forwarded;
      if (active && (!scepad_haptics_active || scepad_packets_forwarded % 64 == 1)) {
        logs::log(logs::debug,
                  "[SCEPAD_AUDIO] forwarding nonzero rear-channel PCM packet {} for session {}",
                  scepad_packets_forwarded,
                  scepad_session_id);
      }
    }
    scepad_haptics_active = active;
    scepad_pcm.erase(scepad_pcm.begin(), scepad_pcm.begin() + input_frames_per_packet * 4);
  }
}

} // namespace wolf::core::audio
