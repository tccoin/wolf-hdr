#!/usr/bin/env bash
set -Eeuo pipefail
export HOME=/home/retro
export BWRAP=/usr/bin/bwrap
# The Selkies base advertises a system bus started by its own supervisor.
# Wolf starts only a session bus, so that inherited system-bus socket does not
# exist. pressure-vessel otherwise tries to bind it and aborts before Wine
# (including Heroic's winepath/fake Epic launcher preparation) can run.
# Keep DBUS_SESSION_BUS_ADDRESS: it belongs to the actual KDE/Gamescope session.
unset DBUS_SYSTEM_BUS_ADDRESS
# Accept old desktop shortcuts without nesting the launcher in Gamescope.
if [ "${1:-}" = --wolf-hdr ]; then shift; fi
notify_error() {
  echo "$*" >&2
  command -v kdialog >/dev/null && kdialog --error "$*" || true
}
if ! python3 /usr/local/share/wolf/heroic-profile-init.py; then
  notify_error 'Heroic profile conflicts with existing data. Check the shared profile mount; nothing was overwritten.'
  exit 1
fi
# The profile/UMU/game paths are symlinks into the optional shared mount.
# pressure-vessel sees HOME but does not automatically expose those external
# symlink targets. UMU overwrites STEAM_COMPAT_MOUNTS, so use its runtime's
# additive filesystem variable to keep the selected shared home reachable.
shared_home="${WOLF_HEROIC_SHARED_HOME:-/var/lib/wolf-shared-heroic}"
if [ -d "$shared_home" ]; then
  shared_home="$(readlink -f "$shared_home")"
  export PRESSURE_VESSEL_FILESYSTEMS_RW="${PRESSURE_VESSEL_FILESYSTEMS_RW:+${PRESSURE_VESSEL_FILESYSTEMS_RW}:}$shared_home"
fi
profile="$HOME/.config/heroic"
mkdir -p "$profile"
exec 9>"$profile/.wolf-kde-launcher.lock"
if ! flock -n 9; then
  notify_error 'Heroic or a launched game is still running. Exit it before switching launch modes.'
  exit 1
fi
# Electron locks contain the previous container hostname. Only recover a lock
# from a recorded managed session after its lifetime flock has been released.
previous_host=''
owner_file="$profile/.wolf-kde-owner"
if [ -f "$owner_file" ]; then read -r previous_host < "$owner_file" || true; fi
if [[ "$previous_host" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$ ]] && \
   [ -L "$profile/SingletonLock" ] && \
   [[ "$(readlink "$profile/SingletonLock")" == "$previous_host-"* ]]; then
  archive="$(mktemp -d "$profile/wolf-stale-locks.XXXXXX")"
  for name in SingletonLock SingletonSocket SingletonCookie; do
    [ ! -L "$profile/$name" ] || mv -- "$profile/$name" "$archive/"
  done
fi
hostname > "$owner_file"
if ! python3 /usr/local/share/wolf/heroic-game-defaults.py; then
  notify_error 'Could not configure the Heroic game launcher. Existing settings were preserved; check the launch log.'
  exit 1
fi
# Use the Linux evdev path verified against the actual Wolf DualSense.
export SDL_JOYSTICK_HIDAPI="${SDL_JOYSTICK_HIDAPI:-0}"
if [ "$SDL_JOYSTICK_HIDAPI" = "0" ]; then
  export SDL_GAMECONTROLLERCONFIG='0500a8394c050000e60c000011810000,Wolf DualSense,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,guide:b10,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,'
fi
# Keep Electron and prefix maintenance on KDE. Only actual game launches use
# the wrapper, before UMU creates its runtime and Wine connects to XWayland.
export WOLF_HEROIC_WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-}"
unset PROTON_ENABLE_WAYLAND DISABLE_HDR_WSI VK_INSTANCE_LAYERS
unset PROTON_ENABLE_HDR DXVK_HDR ENABLE_GAMESCOPE_WSI ENABLE_HDR_WSI
unset GAMESCOPE_WSI_OVERLAY_BOOTSTRAP GAMESCOPE_WAYLAND_DISPLAY
# Heroic runs Wine prefix maintenance (wineboot/winepath) before its game
# wrapper. Wine caches the monitor's HDR capability during that maintenance;
# enabling DXVK_HDR only in the later Gamescope child leaves Advanced Color
# disabled even though Vulkan already exposes HDR formats. These Wine-only
# flags must be present early; keep the Vulkan WSI layer game-only.
if [ "${WOLF_KDE_ENABLE_HDR:-0}" = 1 ] && [ "${WOLF_HEROIC_GAME_HDR:-1}" != 0 ]; then
  export DXVK_HDR=1 PROTON_ENABLE_HDR=1
fi
/opt/Heroic/heroic --ozone-platform=x11 "$@"
