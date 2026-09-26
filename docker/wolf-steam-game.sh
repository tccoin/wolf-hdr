#!/usr/bin/env bash
# The Steam desktop default game wrapper. Steam invokes it via LaunchOptions
# as: wolf-steam-game --appid ID -- GAME_COMMAND [existing game arguments].
set -Eeuo pipefail

if [ "${1:-}" != --appid ] || ! [[ "${2:-}" =~ ^[0-9]+$ ]] || [ "${3:-}" != -- ] || [ "$#" -lt 4 ]; then
  echo 'Usage: wolf-steam-game --appid STEAM_APP_ID -- COMMAND [ARGUMENT...]' >&2
  exit 2
fi
game_appid="${2}"
shift 3

# Steam's pressure-vessel runtime builds a private /dev for Proton. Wolf has
# already scoped the session's evdev/hidraw nodes to this app container, but
# without forwarding them once more here native Windows gamepad APIs cannot
# open a DualSense (Steam itself can still see it outside the sandbox).
# Do not forward the parent /dev: pressure-vessel can propagate its private
# devtmpfs back to the KDE session and make the session's dynamic input nodes
# disappear after a game exits.
pressure_vessel_rw="${PRESSURE_VESSEL_FILESYSTEMS_RW:-}"
for input_path in /dev/input /dev/uinput /dev/hidraw*; do
  [ -e "$input_path" ] || continue
  pressure_vessel_rw="${pressure_vessel_rw:+${pressure_vessel_rw}:}${input_path}"
done
export PRESSURE_VESSEL_FILESYSTEMS_RW="$pressure_vessel_rw"
if [ -d /run/udev ]; then
  export PRESSURE_VESSEL_FILESYSTEMS_RO="${PRESSURE_VESSEL_FILESYSTEMS_RO:+${PRESSURE_VESSEL_FILESYSTEMS_RO}:}/run/udev"
fi

# Wolf captures DualSense haptic PCM from its named PulseAudio quad sink.
# Prefer that transport for every Proton game: a local PipeWire/ALSA endpoint
# would bypass the streaming server and therefore never reach Moonlight.
export WOLF_SCEPAD_FORCE_PULSE=1
# Expose Sony's non-event ScePad capability to every Proton title.  The Wine
# patch above keeps its four-channel feedback stream on Wolf's Pulse sink, so
# it remains routable to Moonlight instead of being sent to a host-local ALSA
# endpoint.  Games that only use ordinary rumble or do not support ScePad are
# unaffected.
export PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT=1
export PROTON_SONY_WINDOWS_DEVICE_NAMES=1

# Keep NVIDIA's optional NVAPI path available for CONTROL Resonant's DLSS
# features. Let Proton seed/update the per-prefix NGX runtime as well: the game
# ships Streamline plugin DLLs, but the captured launch did not load any NGX
# implementation DLL. Do not mask DXR here: ray tracing must remain selectable.
if [[ "$game_appid" == 3669870 ]]; then
  # CONTROL requests Sony's four-channel ScePad endpoint.  Keep the route in
  # the wrapper rather than Steam LaunchOptions: Steam's global HDR defaults
  # reconcile LaunchOptions before startup and would otherwise append these
  # variables after %command%, where they cannot affect Proton.
  # WineBus keeps the virtual HID's normal dynamic ContainerId for GameInput.
  # WinePulse pairs the active native controller with Wolf's unique named
  # endpoint (docker/pulse/wolf-scepad.pa) rather than forcing a HID identity.
  # Enables Wwise's non-event ScePad path. WinePulse sends its four-channel
  # audio through control_dualsense_audio, which Wolf forwards to Moonlight.
  # Deep Wine diagnostics are expensive and can perturb timing.  Enable them
  # only for a one-off investigation, never as the default game launch path.
  # Steam's URL launcher does not reliably preserve one-shot environment
  # variables. A marker in the per-session runtime directory provides the
  # same opt-in diagnostics without making tracing part of normal launches.
  if [[ "${WOLF_CONTROL_SCEPAD_TRACE:-0}" == 1 ||
        -e "${XDG_RUNTIME_DIR:-/tmp}/wolf-control-scepad-trace" ]]; then
    export WINEDEBUG="${WINEDEBUG:+${WINEDEBUG},}+hid,+mmdevapi,+ginput,+xaudio2,+pulse,+loaddll"
  fi
  export PROTON_ENABLE_NVAPI=1
  export PROTON_ENABLE_NGX_UPDATER=1
  # This title stores Ray Reconstruction and Frame Generation as disabled in
  # renderer.ini by default, even when the RTX features are available. Use
  # DXVK-NVAPI's per-game NGX overrides so SR/RR/FG are exposed together; the
  # 4090 uses standard (2x) DLSS FG, not 50-series multi-frame generation.
  export DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE=on
  export DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE=on
  export DXVK_NVAPI_DRS_NGX_DLSS_FG_OVERRIDE=on
  export DXVK_NVAPI_DRS_NGX_DLSS_SR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_latest
  export DXVK_NVAPI_DRS_NGX_DLSS_RR_OVERRIDE_RENDER_PRESET_SELECTION=render_preset_latest
fi

# Big Picture already owns a fullscreen HDR Gamescope. Do not nest another
# compositor: that would break focus/overlay behavior and add latency.
if [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ] || [ "${WOLF_KDE_STEAM_MODE:-}" = big-picture ]; then
  exec "$@"
fi

if [ "${WOLF_KDE_ENABLE_HDR:-0}" != 1 ] || [ "${WOLF_STEAM_GAME_HDR:-1}" = 0 ]; then
  exec "$@"
fi

if [ -z "${WAYLAND_DISPLAY:-}" ]; then
  echo 'Steam HDR: no KDE Wayland display is available.' >&2
  exit 1
fi

export BWRAP=/usr/bin/bwrap
unset DBUS_SYSTEM_BUS_ADDRESS PROTON_ENABLE_WAYLAND DISABLE_HDR_WSI VK_INSTANCE_LAYERS
export PROTON_ENABLE_HDR=1 DXVK_HDR=1 ENABLE_GAMESCOPE_WSI=1 ENABLE_HDR_WSI=1
export GAMESCOPE_PATCHED_EDID_FILE="$XDG_RUNTIME_DIR/wolf-steam-game-${game_appid}-$$.bin"
# Give Steam Overlay one initial composited frame to attach to the game's
# XWayland window, then Gamescope returns to its normal direct path.  Without
# this, a desktop-Steam game is launched straight into a nested Gamescope and
# the Guide button can be accepted by Steam but never surface its overlay.
export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP="${WOLF_GAMESCOPE_WSI_OVERLAY_BOOTSTRAP:-1}"
export VKD3D_DISABLE_EXTENSIONS="${VKD3D_DISABLE_EXTENSIONS:+${VKD3D_DISABLE_EXTENSIONS},}VK_EXT_present_timing"

# Desktop Steam is an ordinary KWin window. Non-Steam focus policy (-e) would
# hide games that have no Steam game-id on their XWayland window, so use the
# normal single-application connector strategy here.
exec /usr/games/gamescope --backend wayland -f --hdr-enabled \
  --virtual-connector-strategy SingleApplication \
  --hdr-sdr-content-nits "${WOLF_SDR_REFERENCE_WHITE:-100}" \
  -W "${GAMESCOPE_WIDTH:-1920}" -H "${GAMESCOPE_HEIGHT:-1080}" \
  -w "${GAMESCOPE_WIDTH:-1920}" -h "${GAMESCOPE_HEIGHT:-1080}" \
  -r "${GAMESCOPE_REFRESH:-60}" -- \
  env VK_INSTANCE_LAYERS=VK_LAYER_FROG_gamescope_wsi_x86_64 "$@"
