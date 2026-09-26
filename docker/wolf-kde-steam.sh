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
# A previous diagnostic or a manually invoked launcher can leave these
# regenerable files owned by root.  flock only needs a readable descriptor;
# opening without O_TRUNC lets the normal desktop user recover that stale
# state instead of preventing Steam from starting before it reaches Steam.
transition_lock="${XDG_RUNTIME_DIR:?}/wolf-kde-steam-transition.lock"
startup_lock="$XDG_RUNTIME_DIR/wolf-kde-steam-v2.lock"
[ -e "$transition_lock" ] || : >"$transition_lock"
[ -e "$startup_lock" ] || : >"$startup_lock"
exec 8<"$transition_lock"
if ! flock -w 45 8; then
  notify_error "Steam is switching modes. Please try again shortly."
  exit 1
fi
# v2 also migrates away from the old inode inherited by external browsers.
exec 9<"$startup_lock"

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
  while read -r candidate; do
    [ -n "$candidate" ] || continue
    state=$(ps -o stat= -p "$candidate" 2>/dev/null | tr -d ' ')
    case "$state" in Z*|'') continue ;; esac
    pid=$candidate
    break
  done < <(pgrep -u "$(id -u)" -x steam || true)
  [ -n "$pid" ] && break
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

# Steam has no global Linux game launch option. Before starting a *desktop*
# client, install our default wrapper in the active account's installed-game
# entries. It preserves custom launch arguments after %command%; never edit a
# live Steam config, and never alter Big Picture's outer Gamescope session.
if [ -z "$pid" ] && [ "$requested_mode" != big-picture ]; then
  if ! python3 /usr/local/share/wolf/steam-game-defaults.py; then
    notify_error "Could not apply Steam HDR game defaults. Steam was not started; inspect wolf-selkies-steam.log."
    exit 1
  fi
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

# Keep SDL on the Linux evdev path. It is the path verified against the actual
# Wolf pad and avoids HIDAPI's inconsistent controller visibility in Steam.
export SDL_JOYSTICK_HIDAPI="${SDL_JOYSTICK_HIDAPI:-0}"
if [ "$SDL_JOYSTICK_HIDAPI" = "0" ]; then
  export SDL_GAMECONTROLLERCONFIG="0500a8394c050000e60c000011810000,Wolf DualSense,a:b0,b:b1,x:b3,y:b2,back:b8,start:b9,guide:b10,leftshoulder:b4,rightshoulder:b5,leftstick:b11,rightstick:b12,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,platform:Linux,"
fi
export PROTON_ENABLE_HDR=1 DXVK_HDR=1 PROTON_USE_XALIA=0

if [ "$WOLF_KDE_STEAM_MODE" = big-picture ] && [ "${WOLF_KDE_GAMESCOPE_HDR:-0}" = 1 ]; then
  # Gamescope publishes this EDID path to Proton on its XWayland root window.
  # Without it, DXVK invents a 1499-nit display even when Wolf advertises 1000.
  export GAMESCOPE_PATCHED_EDID_FILE="$XDG_RUNTIME_DIR/wolf-steam-gamescope-edid.bin"
  # Experimental 11 / vkd3d 3.1 queries a null swapchain through present_timing
  # when focus changes to Overlay (winevulkan +0x2a685). Keep Overlay/HDR and
  # the normal KHR_present_wait path, but avoid that optional timing path.
  export VKD3D_DISABLE_EXTENSIONS="${VKD3D_DISABLE_EXTENSIONS:+${VKD3D_DISABLE_EXTENSIONS},}VK_EXT_present_timing"
  export ENABLE_GAMESCOPE_WSI=1 ENABLE_HDR_WSI=1
  # Keep the game's initial XWayland swapchain composited long enough for the
  # Steam Overlay to discover and attach to it. If Gamescope WSI bypasses
  # XWayland immediately, Proton can receive an Overlay-activated callback
  # without the matching deactivation callback on return from Big Picture;
  # Wine then suppresses all keyboard and controller input for the game.
  # The bootstrap only lasts ten seconds and does not change the HDR mode.
  export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP=1
  export STEAM_GAMESCOPE_COLOR_MANAGED=1 STEAM_GAMESCOPE_VIRTUAL_WHITE=1
  export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
  export VK_LOADER_LAYERS_DISABLE="${VK_LOADER_LAYERS_DISABLE:+${VK_LOADER_LAYERS_DISABLE},}VK_LAYER_VALVE_steam_fossilize_64"
  unset PROTON_ENABLE_WAYLAND
  export WAYLAND_DISPLAY=wayland-kde DISPLAY=:0
  gamescope_debug_focus_args=()
  if [ "${WOLF_KDE_GAMESCOPE_DEBUG_FOCUS:-0}" = 1 ]; then
    gamescope_debug_focus_args+=(--debug-focus)
  fi
  # Big Picture owns a fullscreen native-resolution Gamescope surface.
  # Desktop Steam below is a direct KWin client, without an outer compositor.
  # -e is required for Steam's Big Picture focus/overlay routing under the
  # nested compositor. Keep the lifetime lock in this launcher only; Steam can
  # spawn persistent browsers that must not hold it after Steam exits.
  # browsers; they must not keep the lock after Steam itself has exited.
  /usr/games/gamescope "${gamescope_debug_focus_args[@]}" --backend wayland -e -f --hdr-enabled \
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
