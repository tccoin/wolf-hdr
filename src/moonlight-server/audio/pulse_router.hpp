#pragma once

#include <events/events.hpp>
#include <immer/box.hpp>
#include <immer/vector.hpp>

#include <cstdint>
#include <mutex>
#include <optional>
#include <pulse/pulseaudio.h>
#include <state/sessions.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct pa_context;
struct pa_sink_input_info;

namespace wolf::core::audio {

struct Server;

/**
 * PulseAudio routing state.
 * Kept alive by the caller together with the returned handler registrations.
 */
struct PulseAudioRouterState {
  std::shared_ptr<audio::Server> pulse_server;

  // hostname -> session_id
  immer::atom<immer::map<std::string, std::string>> host_to_session{immer::map<std::string, std::string>{}};

  // session_id -> sink_index (from VirtualAudioSinkCreated)
  immer::atom<immer::map<std::string, uint32_t>> session_to_sink_idx{immer::map<std::string, uint32_t>{}};

  // A KDE/Steam game runs in a lobby, while the encrypted Moonlight control
  // channel is owned by the numeric stream session that joined that lobby.
  // Keep this relation so ScePad audio reaches the actual controller instead
  // of being queued against the lobby UUID.
  immer::atom<immer::map<std::string, std::string>> lobby_to_moonlight_session{immer::map<std::string, std::string>{}};

  // The virtual USB DualSense exposes a 48 kHz quad endpoint.  Its rear pair
  // is the left/right haptic actuator data, not desktop audio.
  uint32_t scepad_sink_idx = PA_INVALID_INDEX;
  pa_stream *scepad_monitor = nullptr;
  // The quad stream belongs to a KDE lobby/container, while the destination
  // control channel has a new numeric session id after every reconnect.
  // Retain the owner lobby so on_lobby_joined() can rebind an already-open
  // Wwise stream without requiring the game to recreate its audio endpoint.
  std::string scepad_lobby_id;
  std::string scepad_session_id;
  std::vector<float> scepad_pcm;
  bool scepad_haptics_active = false;
  std::uint64_t scepad_packets_forwarded = 0;

  // Pulse subscribe / callbacks
  void enable_pulse_subscribe();
  void rescan();

  static void pa_subscribe_cb(pa_context *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata);
  static void pa_sink_input_info_cb(pa_context *c, const pa_sink_input_info *info, int eol, void *userdata);
  static void pa_sink_info_cb(pa_context *c, const pa_sink_info *info, int eol, void *userdata);
  static void pa_scepad_read_cb(pa_stream *stream, size_t length, void *userdata);

  void on_container_created(const events::DockerContainerCreated &ev);
  void on_container_stopped(const events::DockerContainerStopped &ev);
  void on_lobby_joined(const events::JoinLobbyEvent &ev);
  void on_lobby_left(const events::LeaveLobbyEvent &ev);

  void route_sink_input_(pa_context *c, const pa_sink_input_info *info);
  void start_scepad_monitor_(pa_context *c);
  void consume_scepad_pcm_(const float *samples, size_t frames);
};

/**
 * Registers event handlers needed for routing, and enables PulseAudio subscribe.
 *
 * IMPORTANT:
 * - The returned handlers must be kept alive, otherwise handlers unregister.
 * - `state` must be kept alive as long as PulseAudio callbacks can fire.
 */
immer::vector<immer::box<events::EventBusHandlers>>
setup_pulseaudio_router_handlers(const immer::box<state::AppState> &app_state,
                                 std::shared_ptr<PulseAudioRouterState> state);

} // namespace wolf::core::audio
