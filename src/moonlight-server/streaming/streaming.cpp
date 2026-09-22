#include "platforms/hw.hpp"

#include <atomic>
#include <chrono>
#include <control/control.hpp>
#include <core/batched_send.hpp>
#include <gst-video-context.hpp>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <helpers/utils.hpp>
#include <immer/array.hpp>
#include <immer/box.hpp>
#include <memory>
#include <optional>
#include <streaming/streaming.hpp>
#include <thread>

namespace streaming {

using namespace wolf::core::gstreamer;
using namespace wolf::core;

struct GstBusData {
  std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready;
  gst_element_ptr wayland_plugin;
};

gboolean structure_each(GQuark field_id, const GValue *value, gpointer user_data) {
  auto field_str = std::string(g_quark_to_string(field_id));
  if (!G_VALUE_HOLDS_STRING(value)) {
    logs::log(logs::warning, "Wayland source message: {} = {}", field_str, "not a string");
    return FALSE;
  }
  auto value_str = g_value_get_string(value);
  logs::log(logs::debug, "Wayland source message: {} = {}", field_str, value_str);

  if (field_str == "WAYLAND_DISPLAY") {
    logs::log(logs::info, "Wayland display ready, listening on: {}", value_str);
    auto bus_data = static_cast<GstBusData *>(user_data);
    bus_data->on_ready->set_value(
        WaylandDisplayReady{.wayland_socket_name = value_str, .wayland_plugin = bus_data->wayland_plugin});
  }

  return TRUE;
}

static void application_message_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto structure = gst_message_get_structure(msg);
  if (gst_structure_has_name(structure, "wayland.src")) {
    gst_structure_foreach(structure, structure_each, data);
  }
}

struct NeedContextData {
  const std::string device_path;
  std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> gst_context;
};

static void need_context_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  auto ctx_data = static_cast<NeedContextData *>(data);
  if (auto gst_context = ctx_data->gst_context->load().get()) {
    logs::log(logs::debug, "Context already set, passing it to the pipeline.");
    gst_video_context::set_context(gst_context, msg);
  } else if (auto video_context = gst_video_context::need_context_for_device(ctx_data->device_path, msg)) {
    ctx_data->gst_context->store(video_context);
  }
}

static GstBusSyncReply bus_sync_handler(GstBus *bus, GstMessage *msg, gpointer data) {
  if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_NEED_CONTEXT) {
    need_context_handler(bus, msg, data);
  }
  return GST_BUS_PASS;
}

std::string initial_video_colorimetry(bool hdr_requested, events::ColorSpace requested_color_space) {
  if (hdr_requested) {
    // HDR is a connection-level transport contract. SDR producer frames are
    // converted into BT.2020/PQ while native PQ frames pass through.
    return "bt2100-pq";
  }

  switch (requested_color_space) {
  case events::ColorSpace::BT601:
    return "bt601";
  case events::ColorSpace::BT709:
    return "bt709";
  case events::ColorSpace::BT2020:
    return "bt2020";
  }
  return "bt709";
}

std::pair<std::string, std::string> get_color_params(immer::box<events::VideoSession> video_session) {
  std::string color_range = (video_session->color_range == events::ColorRange::JPEG) ? "jpeg" : "mpeg2";
  return std::make_pair(color_range,
                        initial_video_colorimetry(video_session->hdr_requested, video_session->color_space));
}

static void replace_all(std::string &value, std::string_view from, std::string_view to) {
  std::size_t offset = 0;
  while ((offset = value.find(from, offset)) != std::string::npos) {
    value.replace(offset, from.size(), to);
    offset += to.size();
  }
}

std::string prepare_video_pipeline(std::string pipeline, bool hdr_requested) {
  if (hdr_requested) {
    // A live HDR session starts on Wolf UI's GL/RGB10A2 producer, then may
    // join a KDE lobby whose producer is CUDA/P010.  Both represent HDR10,
    // but their Gst memory/caps are intentionally different.  Keep the
    // encoder listener live across that transition and let it negotiate the
    // new producer instead of retaining the initial GL-only caps forever.
    // SDR deliberately retains its existing lifecycle below.
    replace_all(pipeline, "allow-renegotiation=false", "allow-renegotiation=true");

    // The native HDR producer exports a PQ RGB10A2 GL texture. The configured
    // NVENC path already contains one `cudaupload ! cudaconvertscale` stage:
    // that is the required explicit 10-bit RGB-to-P010 conversion. Do not add
    // a second cudaconvertscale here. Apart from changing colours twice on the
    // GL path, the second transform cannot negotiate a P010 CUDA frame from
    // nested KWin after the producer switch.
    const bool native_gl_hdr = std::string(utils::get_env("WOLF_NATIVE_GL_HDR", "0")) == "1" &&
                               pipeline.find("nvh265enc") != std::string::npos;
    if (native_gl_hdr) {
      // All HDR producers normalize to CUDA P010 before their interpipe sink.
      // Do not run a second RGB-to-P010 transform in the listener: the direct
      // GL producer has already converted it and the nested KWin producer is
      // P010 by construction.
      replace_all(pipeline, "cudaupload ! cudaconvertscale add-borders=true ! ", "");
      replace_all(pipeline, "format=NV12", "format=P010_10LE");
      replace_all(pipeline, "colorimetry={color_space}", "colorimetry=bt2100-pq");
      replace_all(pipeline,
                  "format=P010_10LE",
                  "format=P010_10LE, "
                  "mastering-display-info=(string)35400:14600:8500:39850:6550:2300:15635:16450:10000000:1, "
                  "content-light-level=(string)1000:400");
      replace_all(pipeline, "profile=main,", "profile=main-10,");
      return pipeline;
    }

    replace_all(pipeline, "format=NV12", "format=P010_10LE");
    replace_all(pipeline, "profile=main,", "profile=main-10,");
    // Feed a complete HDR10 contract into NVENC. nvh265enc copies these raw
    // caps into the HEVC VUI and static HDR SEI; relying on metadata inherited
    // from the producer is fragile across the interpipe boundary.
    replace_all(pipeline,
                "format=P010_10LE",
                "format=P010_10LE, "
                "mastering-display-info=(string)35400:14600:8500:39850:6550:2300:15635:16450:10000000:1, "
                "content-light-level=(string)1000:400");
  } else {
    // Keep the SDR path exactly on Wolf's established interpipe contract.
    // These restrictions were introduced to freeze the HDR UI-to-app handoff,
    // but retaining them on an SDR encoder leaves a listener registered after
    // the producer ends.  A following Steam connection then receives stale
    // buffers (and gst-interpipe reports an invalid listener).  SDR has no
    // HDR-capability handoff to preserve, so it must accept the ordinary
    // event/caps lifecycle.
    replace_all(pipeline, " format=time", "");
    replace_all(pipeline, " allow-renegotiation=false", "");
    replace_all(pipeline, " accept-events=false", "");
    replace_all(pipeline, " accept-eos-event=false", "");

    // The producer converts its CUDA buffer into ordinary BGRA before the
    // interpipe boundary.  Re-upload it into this encoder pipeline's own CUDA
    // context, then convert to NV12 for NVENC.  Two Wayland producers (Wolf UI
    // and a Steam lobby) own different CUDA buffer pools; passing either pool
    // directly through interpipesrc makes a live listener switch fail caps
    // negotiation.  This is deliberately SDR-only; HDR has a separate P010
    // contract and must remain zero-copy.
    replace_all(pipeline, "cudaupload !", "cudaupload ! cudaconvertscale add-borders=true !");
  }
  return pipeline;
}

std::string producer_caps_for_output(std::string caps, bool hdr_output_requested) {
  if (hdr_output_requested) {
    // An HDR Moonlight connection uses one stable PQ transport contract for its
    // whole lifetime. Wolf UI is replaced by the Steam producer after the encoder
    // pipeline already exists. Starting that producer as BT.709 asks CUDA to
    // renegotiate a live BGRA-to-P010 pipeline and disconnects the client.
    replace_all(caps, ", colorimetry=bt709", "");
    replace_all(caps, ",colorimetry=bt709", "");
    if (caps.find("memory:CUDAMemory") != std::string::npos && caps.find("colorimetry=") == std::string::npos) {
      caps += ", colorimetry=bt2100-pq";
    }
    // The GL source advertises the RGB HDR transfer via its `wolf-hdr-pq`
    // marker. Its advertised GLMemory caps contain the PQ colour contract, but
    // intentionally do not carry HDR10 static metadata. Requiring mastering
    // display / CLL fields at this producer-side capsfilter asks GStreamer to
    // transform GLMemory into a variant it does not offer, leaving a black
    // stream (or failing with NOT_NEGOTIATED) before the first frame. NVENC
    // receives the same fixed HDR10 metadata after the CUDA RGB-to-P010
    // conversion, where it belongs in the encoded bitstream.
    if (caps.find("memory:GLMemory") != std::string::npos) {
      replace_all(caps, ", mastering-display-info=(string)35400:14600:8500:39850:6550:2300:15635:16450:10000000:1", "");
      replace_all(caps, ", content-light-level=(string)1000:400", "");
    }
    return caps;
  }
  if (!hdr_output_requested) {
    // The native GL bridge is deliberately HDR-only. An SDR Moonlight request
    // continues through Wolf's established BGRA/CUDA conversion path; H.264
    // cannot consume RGB10A2 and must not inherit PQ producer caps.
    if (caps.find("memory:GLMemory") != std::string::npos) {
      return "video/x-raw(memory:CUDAMemory), format=BGRA";
    }
    // This HDR override contains a P010 Vulkan download. Do not run it for SDR: return to the
    // ordinary Wolf raw producer, matched by the native CUDA conversion branch above.
    if (caps.find("vulkandownload") != std::string::npos) {
      return "video/x-raw";
    }
    replace_all(caps, "P010_10LE", "NV12");
    replace_all(caps, "drm-format=P010", "drm-format=NV12");
    replace_all(caps, "colorimetry=bt2100-pq", "colorimetry=bt709");
  }
  return caps;
}

void start_video_producer(const std::string &session_id,
                          const std::string &buffer_format,
                          const std::string &render_node,
                          const wolf::core::virtual_display::DisplayMode &display_mode,
                          bool hdr_capable,
                          bool restrict_hdr_dmabufs,
                          std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                          std::shared_ptr<boost::promise<WaylandDisplayReady>> on_ready,
                          std::shared_ptr<events::EventBusType> event_bus) {
  // The native Vulkan zero-copy path needs the source in Vulkan mode so it emits NV12
  // memory:VulkanImage (selected when the negotiated producer caps are VulkanImage).
  std::string vulkan_prop = buffer_format.find("VulkanImage") != std::string::npos ? " vulkan=true" : "";
  // GLMemory/RGB10A2 is the explicit native-PQ route. Keep it opt-in from the
  // selected app profile so all ordinary SDR sessions retain Wolf's CUDA path.
  std::string gl_hdr_prop = buffer_format.find("memory:GLMemory") != std::string::npos ? " gl-hdr=true" : "";
  std::string restrict_hdr_dmabufs_prop = restrict_hdr_dmabufs ? " restrict-hdr-dmabufs=true" : "";
  // HDR is an application/producer capability, not an inference from the
  // selected allocation format.
  std::string hdr_prop = hdr_capable ? " hdr=true" : "";
  // Nested KWin must render into an 8-bit surface on this NVIDIA stack, while
  // a Wolf UI HDR session has an already-live P010 listener. Convert the SDR
  // source to PQ with the GL bridge, then to stable P010 transport before the
  // interpipe handoff.  Asking waylanddisplaysrc itself for P010 leaves no
  // compatible source pad and disconnects the Moonlight session.
  const bool nested_sdr_to_p010 = restrict_hdr_dmabufs && hdr_capable &&
                                  buffer_format.find("P010_10LE") != std::string::npos;
  // Keep every HDR producer on one fully specified caps contract from its first
  // negotiation, including across the Wolf UI -> Steam handoff.
  std::string producer_caps = producer_caps_for_output(buffer_format, hdr_capable);
  if (nested_sdr_to_p010) {
    // The restricted compositor still renders into an 8-bit SDR dmabuf.
    // The GL bridge converts sRGB/BT.709 to reference-white-scaled BT.2020/PQ
    // in a separate RGB10A2 texture before CUDA performs RGB -> P010.
    // Merely tagging BGRA as PQ bypasses both transfer and gamut conversion.
    gl_hdr_prop = " gl-hdr=true";
    producer_caps = "video/x-raw(memory:GLMemory), format=RGB10A2_LE";
  }
  // Put the display mode on the first (Vulkan) caps structure as well as on the
  // post-download caps.  If it is only present after vulkandownload, the
  // interpipe handoff can ask waylanddisplaysrc to renegotiate when Wolf UI is
  // replaced by Steam, which stops the producer with not-negotiated.
  const auto first_caps_end = producer_caps.find(" ! ");
  const auto mode_caps = fmt::format(", width={}, height={}, framerate={}/1",
                                     display_mode.width,
                                     display_mode.height,
                                     display_mode.refreshRate);
  if (first_caps_end == std::string::npos) {
    producer_caps += mode_caps;
  } else {
    producer_caps.insert(first_caps_end, mode_caps);
  }
  if (hdr_capable && producer_caps.find("P010_10LE") != std::string::npos) {
    if (producer_caps.find("colorimetry=") == std::string::npos) {
      producer_caps += ", colorimetry=bt2100-pq";
    }
    // Keep static HDR metadata out of the Vulkan-download/interpipe boundary.  The
    // downstream CUDA upload element rejects those extra caps fields while switching
    // from Wolf UI to Steam, even though nvh265enc accepts them.  The encoder pipeline
    // adds the same fixed mastering/CLL metadata after cudaupload, immediately before
    // NVENC, so the bitstream signalling is unchanged and the live handoff stays
    // negotiated.
    replace_all(producer_caps,
                ", mastering-display-info=(string)35400:14600:8500:39850:6550:2300:15635:16450:10000000:1",
                "");
    replace_all(producer_caps, ", content-light-level=(string)1000:400", "");
  }
  // An SDR stream may switch from the Wolf UI producer to a lobby producer
  // without recreating its NVENC pipeline.  Isolate the two producers' CUDA
  // allocation pools at the interpipe boundary so that switch is a plain
  // BGRA caps hand-off instead of a cross-pipeline CUDA-context renegotiation.
  std::string interpipe_bridge;
  const bool native_gl_to_p010 = hdr_capable && producer_caps.find("memory:GLMemory") != std::string::npos;
  if (native_gl_to_p010) {
    // cudaupload accepts GLMemory caps but otherwise may retain the GL
    // allocation.  Force its CUDA/RGB10A2 output before the conversion: this
    // avoids a later P010 capsfilter receiving a GL texture.  Do not use
    // gldownload here; in Wolf's headless compositor it asks for a separate
    // GL app context and fails with EGL_NOT_INITIALIZED.  The forced CUDA
    // upload followed by exactly one conversion establishes the shared HDR
    // P010/PQ interpipe contract used by both direct and KDE producers.
    interpipe_bridge = fmt::format("cudaupload ! video/x-raw(memory:CUDAMemory), format=RGB10A2_LE ! "
                                   "cudaconvertscale add-borders=true ! "
                                   "video/x-raw(memory:CUDAMemory), format=P010_10LE, colorimetry=bt2100-pq, width={}, "
                                   "height={}, framerate={}/1 ! ",
                                   display_mode.width,
                                   display_mode.height,
                                   display_mode.refreshRate);
  } else if (!hdr_capable && producer_caps.find("memory:CUDAMemory") != std::string::npos &&
             producer_caps.find("format=BGRA") != std::string::npos) {
    // cudadownload can otherwise preserve CUDAMemory when its downstream queue
    // accepts either feature.  videoconvert only accepts ordinary raw frames, so
    // it forces the producer-side CUDA download before the interpipe boundary.
    // This keeps the listener switch independent of the CUDA allocation pool
    // belonging to the previous producer.
    interpipe_bridge = fmt::format(
        "cudadownload ! videoconvert ! video/x-raw, format=BGRA, width={}, height={}, framerate={}/1 ! ",
        display_mode.width,
        display_mode.height,
        display_mode.refreshRate);
  }
  auto pipeline = fmt::format(
      "waylanddisplaysrc name=wolf_wayland_source "
      "render_node={render_node}{vulkan_prop}{gl_hdr_prop}{hdr_prop}{restrict_hdr_dmabufs_prop} ! "
      "queue max-size-buffers=2 leaky=downstream ! "
      "{buffer_format} ! {interpipe_bridge}"
      "queue max-size-buffers=2 leaky=downstream ! \n" //
      // The producer is a live source and owns frame pacing. Do not wait on an old
      // producer-clock timestamp when a listener reconnects.
      "interpipesink sync=false async=false name={session_id}_video max-buffers=1", //
      fmt::arg("vulkan_prop", vulkan_prop),
      fmt::arg("gl_hdr_prop", gl_hdr_prop),
      fmt::arg("hdr_prop", hdr_prop),
      fmt::arg("restrict_hdr_dmabufs_prop", restrict_hdr_dmabufs_prop),
      fmt::arg("buffer_format", producer_caps),
      fmt::arg("interpipe_bridge", interpipe_bridge),
      fmt::arg("render_node", render_node),
      fmt::arg("session_id", session_id),
      fmt::arg("width", display_mode.width),
      fmt::arg("height", display_mode.height),
      fmt::arg("fps", display_mode.refreshRate));
  logs::log(logs::debug, "[GSTREAMER] Starting video producer: {}", pipeline);
  auto bus_data_ptr =
      std::make_shared<GstBusData>(GstBusData{.on_ready = std::move(on_ready), .wayland_plugin = nullptr});
  std::shared_ptr<NeedContextData> ctx_data_ptr =
      std::make_shared<NeedContextData>(NeedContextData{.device_path = render_node, .gst_context = video_context});
  auto failure_reported = std::make_shared<std::atomic_bool>(false);
  run_pipeline(
      pipeline,
      [=](auto pipeline) {
        logs::log(logs::debug, "Setting up waylanddisplaysrc");

        auto wayland_plugin_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_wayland_source");
        auto wayland_plugin_ptr = gst_element_ptr(wayland_plugin_el, ::gst_object_unref);
        bus_data_ptr->wayland_plugin.swap(wayland_plugin_ptr);

        auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
        g_signal_connect(bus, "message::application", G_CALLBACK(application_message_handler), bus_data_ptr.get());
        gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
        gst_object_unref(bus);

        auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
            [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
              if (std::to_string(ev->session_id) == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
            [session_id, pipeline](const immer::box<events::StopLobbyEvent> &ev) {
              if (ev->lobby_id == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler),
                                                                  std::move(stop_lobby_handler)};
      },
      [session_id, event_bus, failure_reported](const std::string &error) {
        if (!failure_reported->exchange(true)) {
          logs::log(logs::error, "[GSTREAMER] Video producer {} failed: {}", session_id, error);
          event_bus->fire_event(immer::box<events::PipelineFailedEvent>{
              events::PipelineFailedEvent{.source_id = session_id, .pipeline = "video-producer", .error = error}});
        }
      });
}

void start_audio_producer(const std::string &session_id,
                          const std::shared_ptr<events::EventBusType> &event_bus,
                          int channel_count,
                          const std::string &sink_name,
                          const std::string &server_name) {
  std::string channel_mask;
  switch (channel_count) {
  case 2:
    channel_mask = "0x3";
    break;
  case 6:
    channel_mask = "0x3f";
    break;
  case 8:
    channel_mask = "0xc3f";
    break;
  default:
    channel_mask = "";
  }

  auto pipeline = fmt::format("pulsesrc device=\"{sink_name}\" server=\"{server_name}\" ! "                           //
                              "audio/x-raw, channels={channels}, channel-mask=(bitmask){channel_mask}, rate=48000 ! " //
                              "queue leaky=downstream max-size-buffers=3 ! "                                          //
                              "interpipesink name=\"{session_id}_audio\" sync=true async=false max-buffers=3",
                              fmt::arg("session_id", session_id),
                              fmt::arg("channels", channel_count),
                              fmt::arg("channel_mask", channel_mask),
                              fmt::arg("sink_name", sink_name),
                              fmt::arg("server_name", server_name));
  logs::log(logs::debug, "[GSTREAMER] Starting audio producer: {}", pipeline);

  auto failure_reported = std::make_shared<std::atomic_bool>(false);
  run_pipeline(
      pipeline,
      [=](auto pipeline) {
        auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
            [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
              if (std::to_string(ev->session_id) == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping audio producer: {}", session_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        auto stop_lobby_handler = event_bus->register_handler<immer::box<events::StopLobbyEvent>>(
            [session_id, pipeline](const immer::box<events::StopLobbyEvent> &ev) {
              if (ev->lobby_id == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping video producer: {}", session_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        return immer::array<immer::box<events::EventBusHandlers>>{std::move(stop_handler),
                                                                  std::move(stop_lobby_handler)};
      },
      [session_id, event_bus, failure_reported](const std::string &error) {
        if (!failure_reported->exchange(true)) {
          logs::log(logs::error, "[GSTREAMER] Audio producer {} failed: {}", session_id, error);
          event_bus->fire_event(immer::box<events::PipelineFailedEvent>{
              events::PipelineFailedEvent{.source_id = session_id, .pipeline = "audio-producer", .error = error}});
        }
      });
}

namespace custom_sink {

struct PacingConfig {
  bool enabled = false;
  std::size_t max_packets_per_ms = 0;
  /**
   * Maximum number of packets per sendmmsg() syscall.
   * Caps batch size to stay under 64KB per call, following Sunshine's pattern.
   * Computed at runtime as min(16, 65536 / packet_size).
   * Does not affect pacing rate — only syscall granularity.
   */
  std::size_t max_batch_size = 16;
  std::chrono::steady_clock::time_point next_frame_start = std::chrono::steady_clock::now();
};

struct UDPSink {
  std::shared_ptr<udp::socket> socket;
  std::shared_ptr<udp::endpoint> client_endpoint;
  PacingConfig pacing;
  wolf::platform::batched_send_info_t send_info;
};

static void ensure_socket_open(UDPSink *udp_sink, bool is_video) {
  if (!udp_sink->socket->is_open()) {
    logs::log(logs::warning, "UDP Socket is not open");
    udp_sink->socket->open(udp::v4());
    wolf::platform::configure_socket_for_streaming(*udp_sink->socket, is_video);
    wolf::platform::enable_socket_qos(udp_sink->socket->native_handle(), is_video);
  }
}

static GstFlowReturn send_buffer_batched(GstBufferList *buffer_list, UDPSink *udp_sink) {
  guint num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0) {
    return GST_FLOW_OK;
  }

  ensure_socket_open(udp_sink, true);

  std::vector<std::pair<GstBuffer *, GstMapInfo>> mapped_buffers;
  udp_sink->send_info.payload_buffers.resize(num_buffers);
  mapped_buffers.reserve(num_buffers);

  for (guint i = 0; i < num_buffers; i++) {
    GstBuffer *buffer = gst_buffer_list_get(buffer_list, i);

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      logs::log(logs::error, "Failed to map buffer {} in batch", i);
      for (auto &[mapped_buffer, m] : mapped_buffers) {
        gst_buffer_unmap(mapped_buffer, &m);
      }
      return GST_FLOW_ERROR;
    }
    mapped_buffers.emplace_back(buffer, map);
    udp_sink->send_info.payload_buffers[i] =
        wolf::platform::buffer_descriptor_t(reinterpret_cast<const char *>(map.data), map.size);
  }

  udp_sink->send_info.native_socket = udp_sink->socket->native_handle();
  udp_sink->send_info.target_address = udp_sink->client_endpoint->address();
  udp_sink->send_info.target_port = udp_sink->client_endpoint->port();

  bool success = true;

  if (!udp_sink->pacing.enabled || num_buffers <= udp_sink->pacing.max_batch_size) {
    udp_sink->send_info.block_offset = 0;
    udp_sink->send_info.block_count = num_buffers;
    success = wolf::platform::send_batch(udp_sink->send_info);
  } else {
    auto &pacing = udp_sink->pacing;
    auto frame_start = std::max(pacing.next_frame_start, std::chrono::steady_clock::now());
    std::size_t packets_sent = 0;
    std::size_t packets_in_window = 0;

    while (packets_sent < num_buffers) {
      std::size_t remaining = num_buffers - packets_sent;
      std::size_t budget = pacing.max_packets_per_ms - packets_in_window;

      if (budget == 0) {
        auto due = frame_start + std::chrono::nanoseconds(1000000) * packets_sent / pacing.max_packets_per_ms;
        auto now = std::chrono::steady_clock::now();
        if (now < due) {
          std::this_thread::sleep_until(due);
        }
        packets_in_window = 0;
        continue;
      }

      std::size_t batch = std::min({budget, pacing.max_batch_size, remaining});

      udp_sink->send_info.block_offset = packets_sent;
      udp_sink->send_info.block_count = batch;
      if (!wolf::platform::send_batch(udp_sink->send_info)) {
        success = false;
        break;
      }

      packets_sent += batch;
      packets_in_window += batch;
    }

    pacing.next_frame_start = frame_start +
                              std::chrono::nanoseconds(1000000) * packets_sent / pacing.max_packets_per_ms;
  }

  for (auto &[buffer, m] : mapped_buffers) {
    gst_buffer_unmap(buffer, &m);
  }

  if (!success) {
    logs::log(logs::warning, "Failed to send batch of {} packets", num_buffers);
    return GST_FLOW_ERROR;
  }

  return GST_FLOW_OK;
}

static GstFlowReturn send_buffer_single(GstBuffer *buffer, UDPSink *udp_sink) {
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    ensure_socket_open(udp_sink, false);

    wolf::platform::batched_send_info_t send_info;
    send_info.payload_buffers.emplace_back(reinterpret_cast<const char *>(map.data), map.size);
    send_info.block_offset = 0;
    send_info.block_count = 1;
    send_info.native_socket = udp_sink->socket->native_handle();
    send_info.target_address = udp_sink->client_endpoint->address();
    send_info.target_port = udp_sink->client_endpoint->port();

    bool success = wolf::platform::send_batch(send_info);
    gst_buffer_unmap(buffer, &map);

    if (!success) {
      logs::log(logs::error, "Error sending UDP packet");
      return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
  } else {
    logs::log(logs::error, "Failed to map buffer");
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn on_new_sample(GstAppSink *appsink, gpointer user_data) {
  std::shared_ptr<GstSample> sample(gst_app_sink_pull_sample(appsink), gst_sample_unref);
  if (!sample) {
    logs::log(logs::warning, "Custom sink: failed to create sample");
    return GST_FLOW_ERROR;
  }

  UDPSink *udp_sink = static_cast<UDPSink *>(user_data);

  if (GstBufferList *buffer_list = gst_sample_get_buffer_list(sample.get())) {
    return send_buffer_batched(buffer_list, udp_sink);
  } else if (GstBuffer *buffer = gst_sample_get_buffer(sample.get())) {
    return send_buffer_single(buffer, udp_sink);
  } else {
    logs::log(logs::warning, "Custom sink: failed to get buffer");
    return GST_FLOW_ERROR;
  }
}

static void configure_appsink(GstElement *appsink, UDPSink *udp_sink) {
  g_object_set(appsink, "emit-signals", FALSE, NULL);
  g_object_set(appsink, "buffer-list", TRUE, NULL);

  GstAppSinkCallbacks callbacks = {nullptr};
  callbacks.new_sample = on_new_sample;
  gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, udp_sink, nullptr);
}

} // namespace custom_sink

/**
 * Start VIDEO pipeline
 */
void start_streaming_video(immer::box<events::VideoSession> video_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<immer::atom<gst_video_context::gst_context_ptr>> video_context,
                           std::shared_ptr<udp::socket> video_socket) {
  auto [color_range, color_space] = get_color_params(video_session);

  auto pipeline_template = prepare_video_pipeline(video_session->gst_pipeline, video_session->hdr_requested);
  auto pipeline = fmt::format(
      fmt::runtime(pipeline_template),
      fmt::arg("session_id", video_session->session_id),
      fmt::arg("width", video_session->display_mode.width),
      fmt::arg("height", video_session->display_mode.height),
      fmt::arg("fps", video_session->display_mode.refreshRate),
      fmt::arg("bitrate", video_session->bitrate_kbps),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", client_ip),
      fmt::arg("payload_size", video_session->packet_size),
      fmt::arg("fec_percentage", video_session->fec_percentage),
      fmt::arg("min_required_fec_packets", video_session->min_required_fec_packets),
      fmt::arg("slices_per_frame", video_session->slices_per_frame),
      fmt::arg("vbv_buffer_size", video_session->bitrate_kbps / video_session->display_mode.refreshRate),
      // Vulkan encoder tunables (defaults = low-latency streaming preset: fastest quality,
      // single reference, no B-frames -> full VCN throughput). Override per-deployment via env.
      fmt::arg("vk_quality", utils::get_env("WOLF_VULKAN_QUALITY", "0")),
      fmt::arg("vk_ref_frames", utils::get_env("WOLF_VULKAN_REF_FRAMES", "1")),
      fmt::arg("vk_b_frames", utils::get_env("WOLF_VULKAN_B_FRAMES", "0")),
      fmt::arg("color_space", color_space),
      fmt::arg("color_range", color_range),
      // The nvcodec conversion template carries this separately from the
      // encoder profile.  Supplying both keeps the raw CUDA caps and encoded
      // HEVC profile in the same bit depth, including for HDR P010 sessions.
      fmt::arg("pixel_format", video_session->hdr_requested ? "P010_10LE" : "NV12"),
      // The current nvcodec default uses this named field instead of a literal
      // `profile=main`.  It must be resolved here (rather than relying on a
      // string replacement above), otherwise fmt throws "argument not found"
      // as soon as an HEVC session starts.  An HDR transport requires Main10.
      fmt::arg("hevc_profile", video_session->hdr_requested ? "main-10" : "main"),
      fmt::arg("render_node", video_session->render_node),
      fmt::arg("host_port", video_session->port));
  logs::log(logs::debug, "Starting video pipeline: \n{}", pipeline);

  bool enable_pacing = utils::get_env("WOLF_ENABLE_VIDEO_PACING", "TRUE") == std::string("TRUE");
  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = video_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port),
      .pacing = {
          .enabled = enable_pacing,
          .max_packets_per_ms = static_cast<std::size_t>(
              std::max(1L, static_cast<long>(1000000000L * 80 / 100 / 1000 / (video_session->packet_size * 8)))),
          .max_batch_size = std::min<std::size_t>(16, 65536 / video_session->packet_size),
      }});
  std::shared_ptr<NeedContextData> ctx_data_ptr = std::make_shared<NeedContextData>(
      NeedContextData{.device_path = video_session->render_node, .gst_context = video_context});
  auto failure_reported = std::make_shared<std::atomic_bool>(false);
  run_pipeline(
      pipeline,
      [video_session, event_bus, udp_sink, ctx_data_ptr](auto pipeline) {
        if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
          logs::log(logs::debug, "Setting up wolf_udp_sink");
          g_assert(GST_IS_APP_SINK(app_sink_el));
          configure_appsink(app_sink_el, udp_sink.get());
          gst_object_unref(app_sink_el);
        }
        auto bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
        gst_bus_set_sync_handler(bus, bus_sync_handler, ctx_data_ptr.get(), nullptr);
        gst_object_unref(bus);

        /*
         * The force IDR event will be triggered by the control stream.
         * We have to pass this back into the gstreamer pipeline
         * in order to force the encoder to produce a new IDR packet
         */
        auto idr_handler = event_bus->register_handler<immer::box<events::IDRRequestEvent>>(
            [sess_id = video_session->session_id, pipeline](const immer::box<events::IDRRequestEvent> &ctrl_ev) {
              if (ctrl_ev->session_id == sess_id) {
                logs::log(logs::debug, "[GSTREAMER] Forcing IDR");
                // Force IDR event, see: https://github.com/centricular/gstwebrtc-demos/issues/186
                // https://gstreamer.freedesktop.org/documentation/additional/design/keyframe-force.html?gi-language=c
                wolf::core::gstreamer::send_message(
                    pipeline.get(),
                    gst_structure_new("GstForceKeyUnit", "all-headers", G_TYPE_BOOLEAN, TRUE, NULL));
              }
            });

        auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
            [sess_id = video_session->session_id, pipeline](const immer::box<events::PauseStreamEvent> &ev) {
              if (ev->session_id == sess_id) {
                logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", sess_id);

                /**
                 * Unfortunately here we can't just pause the pipeline,
                 * when a pipeline will be resumed there are a lot of breaking changes
                 * like:
                 *  - Client IP:PORT
                 *  - AES key and IV for encrypted payloads
                 *  - Client resolution, framerate, and encoding
                 *
                 *  The only solution is to kill the pipeline and re-create it again
                 * when a resume happens
                 */

                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
            [sess_id = video_session->session_id,
             pipeline](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
              if (switch_ev->session_id == sess_id) {
                logs::log(logs::debug,
                          "[GSTREAMER] Switching video producer pipeline for {} to {}",
                          sess_id,
                          switch_ev->interpipe_src_id);
                /* Grab a reference to the interpipesrc */
                auto pipe_name = fmt::format("interpipesrc_{}_video", sess_id);
                if (auto src = gst_bin_get_by_name(GST_BIN(pipeline.get()), pipe_name.c_str())) {
                  /*
                   * Do not call gst_app_src_set_caps() while the interpipesrc
                   * streaming task is active.  That API takes the AppSrc lock and
                   * can deadlock with the interpipe producer during a lobby switch;
                   * the following producer is allowed to negotiate the fixed
                   * P010/HDR contract instead.
                   */
                  auto video_interpipe = fmt::format("{}_video", switch_ev->interpipe_src_id);
                  g_object_set(src, "listen-to", video_interpipe.c_str(), nullptr);
                  gst_object_unref(src);
                } else {
                  logs::log(logs::error, "[GSTREAMER] Failed to get video interpipesrc for {}", sess_id);
                }
              }
            });

        auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
            [sess_id = video_session->session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
              if (ev->session_id == sess_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", sess_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        return immer::array<immer::box<events::EventBusHandlers>>{std::move(idr_handler),
                                                                  std::move(pause_handler),
                                                                  std::move(switch_producer_handler),
                                                                  std::move(stop_handler)};
      },
      [session_id = video_session->session_id, event_bus, failure_reported](const std::string &error) {
        if (!failure_reported->exchange(true)) {
          logs::log(logs::error, "[GSTREAMER] Video stream {} failed: {}", session_id, error);
          event_bus->fire_event(immer::box<events::PipelineFailedEvent>{
              events::PipelineFailedEvent{.source_id = std::to_string(session_id),
                                          .pipeline = "video-stream",
                                          .error = error}});
        }
      });
}

/**
 * Start AUDIO pipeline
 */
void start_streaming_audio(immer::box<events::AudioSession> audio_session,
                           const std::shared_ptr<events::EventBusType> &event_bus,
                           std::string client_ip,
                           unsigned short client_port,
                           std::shared_ptr<udp::socket> audio_socket,
                           const std::string &sink_name,
                           const std::string &server_name) {
  auto pipeline = fmt::format(
      fmt::runtime(audio_session->gst_pipeline),
      fmt::arg("session_id", audio_session->session_id),
      fmt::arg("channels", audio_session->audio_mode.channels),
      fmt::arg("bitrate", audio_session->audio_mode.bitrate),
      // TODO: opusenc hardcodes those two
      // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.24.6/subprojects/gst-plugins-base/ext/opus/gstopusenc.c#L661-666
      fmt::arg("streams", audio_session->audio_mode.streams),
      fmt::arg("coupled_streams", audio_session->audio_mode.coupled_streams),
      fmt::arg("sink_name", sink_name),
      fmt::arg("server_name", server_name),
      fmt::arg("packet_duration", audio_session->packet_duration),
      fmt::arg("aes_key", audio_session->aes_key),
      fmt::arg("aes_iv", audio_session->aes_iv),
      fmt::arg("encrypt", audio_session->encrypt_audio),
      fmt::arg("client_port", client_port),
      fmt::arg("client_ip", client_ip),
      fmt::arg("host_port", audio_session->port));
  logs::log(logs::debug, "Starting audio pipeline: \n{}", pipeline);

  std::shared_ptr<custom_sink::UDPSink> udp_sink = std::make_shared<custom_sink::UDPSink>(custom_sink::UDPSink{
      .socket = audio_socket,
      .client_endpoint = std::make_shared<udp::endpoint>(boost::asio::ip::make_address(client_ip), client_port)});

  auto failure_reported = std::make_shared<std::atomic_bool>(false);
  run_pipeline(
      pipeline,
      [session_id = audio_session->session_id, udp_sink, event_bus](auto pipeline) {
        if (auto app_sink_el = gst_bin_get_by_name(GST_BIN(pipeline.get()), "wolf_udp_sink")) {
          logs::log(logs::debug, "Setting up wolf_udp_sink");
          g_assert(GST_IS_APP_SINK(app_sink_el));
          custom_sink::configure_appsink(app_sink_el, udp_sink.get());
          gst_object_unref(app_sink_el);
        }

        auto pause_handler = event_bus->register_handler<immer::box<events::PauseStreamEvent>>(
            [session_id, pipeline](const immer::box<events::PauseStreamEvent> &ev) {
              if (ev->session_id == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Pausing pipeline: {}", session_id);

                /**
                 * Unfortunately here we can't just pause the pipeline,
                 * when a pipeline will be resumed there are a lot of breaking changes
                 * like:
                 *  - Client IP:PORT
                 *  - AES key and IV for encrypted payloads
                 *  - Client resolution, framerate, and encoding
                 *
                 *  The only solution is to kill the pipeline and re-create it again
                 * when a resume happens
                 */

                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        auto switch_producer_handler = event_bus->register_handler<immer::box<events::SwitchStreamProducerEvents>>(
            [session_id, pipeline](const immer::box<events::SwitchStreamProducerEvents> &switch_ev) {
              if (switch_ev->session_id == session_id) {
                logs::log(logs::debug,
                          "[GSTREAMER] Switching audio producer for {} to {}",
                          session_id,
                          switch_ev->interpipe_src_id);

                auto pipe_name = fmt::format("interpipesrc_{}_audio", session_id);
                if (auto src = gst_bin_get_by_name(GST_BIN(pipeline.get()), pipe_name.c_str())) {
                  /* Switch without resetting AppSrc caps from the streaming thread. */
                  auto audio_interpipe = fmt::format("{}_audio", switch_ev->interpipe_src_id);
                  g_object_set(src, "listen-to", audio_interpipe.c_str(), nullptr);
                  gst_object_unref(src);
                } else {
                  logs::log(logs::error, "[GSTREAMER] Failed to get audio interpipesrc for {}", session_id);
                }
              }
            });

        auto stop_handler = event_bus->register_handler<immer::box<events::StopStreamEvent>>(
            [session_id, pipeline](const immer::box<events::StopStreamEvent> &ev) {
              if (ev->session_id == session_id) {
                logs::log(logs::debug, "[GSTREAMER] Stopping pipeline: {}", session_id);
                gst_element_send_event(pipeline.get(), gst_event_new_eos());
              }
            });

        return immer::array<immer::box<events::EventBusHandlers>>{std::move(pause_handler),
                                                                  std::move(switch_producer_handler),
                                                                  std::move(stop_handler)};
      },
      [session_id = audio_session->session_id, event_bus, failure_reported](const std::string &error) {
        if (!failure_reported->exchange(true)) {
          logs::log(logs::error, "[GSTREAMER] Audio stream {} failed: {}", session_id, error);
          event_bus->fire_event(immer::box<events::PipelineFailedEvent>{
              events::PipelineFailedEvent{.source_id = std::to_string(session_id),
                                          .pipeline = "audio-stream",
                                          .error = error}});
        }
      });
}

} // namespace streaming
