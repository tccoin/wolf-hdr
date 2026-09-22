#!/usr/bin/env bash
set -Eeuo pipefail

# One entry point for Plasma desktop shortcuts, steam:// URLs, and autostart.
# The appliance launcher bootstraps ~/.steam/steam; Wolf uses ~/.steam itself.
export HOME=/home/retro XDG_DATA_HOME=/home/retro/.local/share
exec >>"$HOME/wolf-selkies-steam.log" 2>&1
unset DBUS_SYSTEM_BUS_ADDRESS
# Use the image's root-owned, setuid bubblewrap with the runner's capabilities.
# The Selkies proot shim cannot satisfy Steam's pressure-vessel requirements.
export BWRAP=/usr/bin/bwrap
/usr/local/bin/wolf-steam-profile-init
steam=/home/retro/.steam/steam.sh
if [ ! -x "$steam" ]; then steam=/home/retro/.steam/steam/steam.sh; fi
if [ ! -x "$steam" ]; then steam=/home/retro/.local/share/Steam/steam.sh; fi
test -x "$steam"

requested_mode=auto
return_to_desktop=0
case "${1:-}" in
  --wolf-desktop) requested_mode=desktop; shift ;;
  --wolf-return-to-desktop) requested_mode=desktop; return_to_desktop=1; shift ;;
  --wolf-big-picture) requested_mode=big-picture; shift ;;
esac
if [ "$#" -eq 0 ]; then
  if [ "$requested_mode" = big-picture ]; then
    set -- steam://open/bigpicture
  else
    set -- steam://open/main
  fi
fi

notify_error() {
  echo "[wolf-kde-steam] $*" >&2
  kdialog --title Steam --error "$*" 8>&- 9>&- &
}

# Serialize mode transitions, separately from the lifetime/startup lock.
exec 8>"${XDG_RUNTIME_DIR:?}/wolf-kde-steam-transition.lock"
if ! flock -w 45 8; then
  notify_error "Steam is switching modes. Please try again shortly."
  exit 1
fi
# v2 also migrates away from the old inode inherited by external browsers.
exec 9>"$XDG_RUNTIME_DIR/wolf-kde-steam-v2.lock"

existing_display() (
  local entry
  while IFS= read -r -d "" entry; do
    case "$entry" in
      DISPLAY=*|XAUTHORITY=*|WAYLAND_DISPLAY=*|GAMESCOPE_WAYLAND_DISPLAY=*) export "$entry" ;;
    esac
  done < "/proc/$pid/environ"
  exec "$@" 8>&- 9>&-
)

pid=""
# A second click can arrive before Steam has forked its real executable.
for _ in {1..150}; do
  pid=$(pgrep -o -u "$(id -u)" -x steam) && break
  flock -n 9 && break
  sleep .1
done
if [ -n "$pid" ]; then
  current_mode=legacy-gamescope
  while IFS= read -r -d "" entry; do
    case "$entry" in WOLF_KDE_STEAM_MODE=*) current_mode=${entry#*=} ;; esac
  done < "/proc/$pid/environ"
  if [ "$requested_mode" != auto ] && [ "$requested_mode" != "$current_mode" ]; then
    # Steam shares one profile/IPC endpoint. Switching display servers needs
    # a graceful restart, never a second concurrent client or a forced kill.
    if ! games=$(python3 /usr/local/share/wolf/steam-running-games.py "$pid"); then
      notify_error "Could not verify whether a game is running. Exit Steam manually before switching modes."
      exit 1
    fi
    if [ -n "$games" ]; then
      notify_error "A game is still running (App ID: $games). Exit the game before switching Steam modes."
      exit 1
    fi
    echo "[wolf-kde-steam] switching $current_mode -> $requested_mode"
    existing_display timeout 20 "$steam" -shutdown || true
    for _ in {1..300}; do
      [ ! -d "/proc/$pid" ] && break
      sleep .1
    done
    if [ -d "/proc/$pid" ]; then
      notify_error "Steam is still running, possibly syncing or awaiting confirmation. Exit Steam and retry; no process was forcibly stopped."
      exit 1
    fi
    pid=""
  fi
fi

# The power-menu action returns to the existing KDE desktop. Do not start a
# new client from a worker that Gamescope's child reaper is about to terminate.
if [ "$return_to_desktop" = 1 ]; then
  exit 0
fi

# An existing Steam may belong to Gamescope or directly to Plasma :0.
# Forward commands to its actual display without starting another compositor or
# running the appliance bootstrap against a different installation directory.
forward_existing() {
  local script_id
  # Restoring an inner X11 window alone does not unminimize the outer KWin
  # window. Raise that window using the desktop session bus before forwarding.
  if script_id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
      /usr/local/share/wolf/kde-raise-steam.js "wolf-steam-raise-$$"); then
    qdbus6 org.kde.KWin "/Scripting/Script${script_id}" org.kde.kwin.Script.run || true
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "wolf-steam-raise-$$" || true
  fi
  existing_display "$steam" "$@"
}
if [ -n "$pid" ]; then
  forward_existing "$@"
  exit $?
fi

# Coalesce double clicks while Steam is starting. The lock lives only in this
# runner, and is released when the compositor and its children exit.
if ! flock -w 15 9; then
  notify_error "Steam is still starting or stopping. Please try again shortly."
  exit 1
fi
exec 8>&-
export WOLF_KDE_STEAM_MODE=${requested_mode/auto/desktop}

# Linux hid-playstation evdev ordering, verified against the actual Wolf pad.
# HIDAPI uses a different GUID and button ordering; never share this mapping.
export SDL_JOYSTICK_HIDAPI=0
export SDL_GAMECONTROLLERCONFIG="0500a8394c050000e60c000011810000,Wolf DualSense,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,guide:b10,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,"
export PROTON_ENABLE_HDR=1 DXVK_HDR=1 PROTON_USE_XALIA=0

if [ "$WOLF_KDE_STEAM_MODE" = big-picture ] && [ "${WOLF_KDE_GAMESCOPE_HDR:-0}" = 1 ]; then
  # Experimental 11 / vkd3d 3.1 queries a null swapchain through present_timing
  # when focus changes to Overlay (winevulkan +0x2a685). Keep Overlay/HDR and
  # the normal KHR_present_wait path, but avoid that optional timing path.
  export VKD3D_DISABLE_EXTENSIONS="${VKD3D_DISABLE_EXTENSIONS:+${VKD3D_DISABLE_EXTENSIONS},}VK_EXT_present_timing"
  export ENABLE_GAMESCOPE_WSI=1 ENABLE_HDR_WSI=1
  # Cyberpunk queries HDR formats once at startup; do not hide them initially.
  export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP=0
  export STEAM_GAMESCOPE_COLOR_MANAGED=1 STEAM_GAMESCOPE_VIRTUAL_WHITE=1
  export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
  export VK_LOADER_LAYERS_DISABLE="${VK_LOADER_LAYERS_DISABLE:+${VK_LOADER_LAYERS_DISABLE},}VK_LAYER_VALVE_steam_fossilize_64"
  unset PROTON_ENABLE_WAYLAND
  export WAYLAND_DISPLAY=wayland-kde DISPLAY=:0
  # Big Picture owns a fullscreen native-resolution Gamescope surface.
  # Desktop Steam below is a direct KWin client, without an outer compositor.
  # -e selects SteamControlled focus by default, which needs Gamepad UI to
  # choose a connector. Desktop Steam supplies no choice and disappears from
  # KWin entirely. Override AFTER -e while retaining Steam/HDR integration.
  # Keep the lifetime lock in this launcher only. Steam can spawn persistent
  # browsers; they must not keep the lock after Steam itself has exited.
  /usr/games/gamescope --backend wayland -e -f --hdr-enabled \
    --virtual-connector-strategy SingleApplication \
    --hdr-sdr-content-nits "${WOLF_SDR_REFERENCE_WHITE:-203}" \
    -W "${GAMESCOPE_WIDTH:-3440}" -H "${GAMESCOPE_HEIGHT:-1440}" \
    -w "${GAMESCOPE_WIDTH:-3440}" -h "${GAMESCOPE_HEIGHT:-1440}" \
    -r "${GAMESCOPE_REFRESH:-60}" -- \
    env VK_INSTANCE_LAYERS=VK_LAYER_FROG_gamescope_wsi_x86_64 PROTON_LOG=1 \
    "$steam" -steamos3 -gamepadui "$@" 9>&-
else
  # Do not leak nested-WSI assumptions into the native desktop client/games.
  unset ENABLE_GAMESCOPE_WSI ENABLE_HDR_WSI GAMESCOPE_WAYLAND_DISPLAY
  unset STEAM_GAMESCOPE_COLOR_MANAGED STEAM_GAMESCOPE_VIRTUAL_WHITE
  unset GAMESCOPE_WSI_OVERLAY_BOOTSTRAP PROTON_ENABLE_WAYLAND
  export WAYLAND_DISPLAY=wayland-kde DISPLAY=:0
  if [ "$WOLF_KDE_STEAM_MODE" = big-picture ]; then
    "$steam" -gamepadui "$@" 9>&-
    exit $?
  fi
  "$steam" -nobigpicture "$@" 9>&-
fi
