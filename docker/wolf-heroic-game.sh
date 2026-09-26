#!/usr/bin/env bash
# Heroic's wrapper runs outside UMU/pressure-vessel, never around Electron.
set -Eeuo pipefail
if [ "$#" -eq 0 ]; then
  echo 'Usage: wolf-heroic-game COMMAND [ARGUMENT...]' >&2
  exit 2
fi
# Keep every Proton/Wine ScePad stream on Wolf's PulseAudio sink.  A raw local
# PipeWire/ALSA route cannot be observed by Wolf and cannot reach Moonlight.
export WOLF_SCEPAD_FORCE_PULSE=1
export PROTON_DUALSENSE_HAPTICS_PREFER_NON_EVENT=1
export PROTON_SONY_WINDOWS_DEVICE_NAMES=1
# Heroic's UMU/pressure-vessel creates its own private /dev.  Forward only
# the Wolf-injected controller nodes into that sandbox, so every Proton game
# can pair the virtual DualSense HID with the ScePad audio endpoint.  Do not
# bind the parent /dev: it can leak the sandbox's devtmpfs back to KDE.
pressure_vessel_rw="${PRESSURE_VESSEL_FILESYSTEMS_RW:-}"
for input_path in /dev/input /dev/uinput /dev/hidraw*; do
  [ -e "$input_path" ] || continue
  pressure_vessel_rw="${pressure_vessel_rw:+${pressure_vessel_rw}:}${input_path}"
done
export PRESSURE_VESSEL_FILESYSTEMS_RW="$pressure_vessel_rw"
if [ -d /run/udev ]; then
  export PRESSURE_VESSEL_FILESYSTEMS_RO="${PRESSURE_VESSEL_FILESYSTEMS_RO:+${PRESSURE_VESSEL_FILESYSTEMS_RO}:}/run/udev"
fi
if [ "${WOLF_KDE_ENABLE_HDR:-0}" != 1 ] || [ "${WOLF_HEROIC_GAME_HDR:-1}" = 0 ]; then
  exec "$@"
fi
# Respect an explicitly configured outer Gamescope instead of nesting it.
if [ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ]; then
  exec "$@"
fi
export WAYLAND_DISPLAY="${WOLF_HEROIC_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-}}"
if [ -z "$WAYLAND_DISPLAY" ]; then
  echo 'Heroic HDR: no KDE Wayland display is available.' >&2
  exit 1
fi
export BWRAP=/usr/bin/bwrap
unset DBUS_SYSTEM_BUS_ADDRESS PROTON_ENABLE_WAYLAND DISABLE_HDR_WSI VK_INSTANCE_LAYERS
export PROTON_ENABLE_HDR=1 DXVK_HDR=1 ENABLE_GAMESCOPE_WSI=1 ENABLE_HDR_WSI=1
export GAMESCOPE_PATCHED_EDID_FILE="$XDG_RUNTIME_DIR/wolf-heroic-game-$$.bin"
export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP=0
export VKD3D_DISABLE_EXTENSIONS="${VKD3D_DISABLE_EXTENSIONS:+${VKD3D_DISABLE_EXTENSIONS},}VK_EXT_present_timing"
# No -e: non-Steam windows need normal Gamescope focus selection. Each game
# owns one KDE window, so Alt+Tab can reach the launcher and the desktop.
exec /usr/games/gamescope --backend wayland -f --hdr-enabled \
  --virtual-connector-strategy SingleApplication \
  --hdr-sdr-content-nits "${WOLF_SDR_REFERENCE_WHITE:-100}" \
  -W "${GAMESCOPE_WIDTH:-1920}" -H "${GAMESCOPE_HEIGHT:-1080}" \
  -w "${GAMESCOPE_WIDTH:-1920}" -h "${GAMESCOPE_HEIGHT:-1080}" \
  -r "${GAMESCOPE_REFRESH:-60}" -- \
  env VK_INSTANCE_LAYERS=VK_LAYER_FROG_gamescope_wsi_x86_64 "$@"
