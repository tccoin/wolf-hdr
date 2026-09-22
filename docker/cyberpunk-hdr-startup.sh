#!/usr/bin/env bash
set -euo pipefail

source /opt/gow/steam-wait-wayland.sh

# Wolf hot-plugs its virtual controller after keyboard and mouse.  Let it be
# present before Gamescope and Steam perform their one-time controller scan.
# Without a controller this only adds the bounded startup wait below.
deadline=$((SECONDS + 12))
while (( SECONDS < deadline )); do
  if [[ -e /dev/input/mouse0 && -e /dev/input/js0 ]]; then
    break
  fi
  sleep 0.1
done

export DXVK_HDR=1
export ENABLE_GAMESCOPE_WSI=1
export ENABLE_HDR_WSI=1
export PROTON_ENABLE_HDR=1
# Gamescope's WSI layer currently creates native-Wayland Wine surfaces with
# `hdrOutput=false`, so it never exposes its HDR formats to the game.  Keep
# Wine on XWayland for this title: Gamescope WSI then receives the real
# GAMESCOPE_HDR_OUTPUT_FEEDBACK and creates the HDR10 PQ swapchain.
unset PROTON_ENABLE_WAYLAND
export PROTON_ENABLE_NGX_UPDATER=1
export VKD3D_DISABLE_EXTENSIONS=VK_KHR_present_wait
# Give Steam Overlay one brief XWayland-composited bootstrap window before
# the patched WSI layer promotes Cyberpunk's recreated swapchain to HDR.
export GAMESCOPE_WSI_OVERLAY_BOOTSTRAP=1

# Steam sanitizes implicit-layer enable variables when it launches a title.
# Explicitly make both layers part of the instance chain instead: the Overlay
# must be closest to the game so it can draw its UI and receive F12, then it
# forwards the surface to Gamescope WSI, which still owns HDR swapchain setup.
# This is also more reliable than relying on Steam's per-process layer toggle.
unset DISABLE_VK_LAYER_VALVE_steam_overlay_1
export ENABLE_VK_LAYER_VALVE_steam_overlay_1=1
export VK_INSTANCE_LAYERS="${VK_INSTANCE_LAYERS:+${VK_INSTANCE_LAYERS}:}VK_LAYER_VALVE_steam_overlay_64:VK_LAYER_FROG_gamescope_wsi_x86_64"
# Fossilize sits above Gamescope WSI in Steam's implicit Vulkan stack and
# prevents the WSI layer from seeing the game's surface creation.  It is only
# the shader-cache capture layer; disabling it for this dedicated HDR tile
# keeps the actual game/driver path intact and lets WSI own the swapchain.
export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
# Steam sets ENABLE_VK_LAYER_VALVE_steam_fossilize_1 again while spawning a
# title, which takes precedence over that layer's legacy disable toggle.  Use
# the Vulkan loader's layer filter as well: it is evaluated before a layer's
# own enable rule, while VK_INSTANCE_LAYERS below/above still forces Gamescope
# WSI to remain in the chain.
export VK_LOADER_LAYERS_DISABLE="${VK_LOADER_LAYERS_DISABLE:+${VK_LOADER_LAYERS_DISABLE},}VK_LAYER_VALVE_steam_fossilize_64"

# The Steam URI starts the game from a background client inside this nested
# Gamescope session, so its usual desktop activation request does not reach
# the new XWayland game window.  Focus that window exactly once when it first
# appears.  A persistent focus loop breaks Steam Overlay because it steals
# focus back immediately after Shift+Tab; this helper exits after the first
# successful activation.
(
  for _ in $(seq 1 240); do
    window_id=$( (DISPLAY=:0 xwininfo -root -tree 2>/dev/null || true) | awk '/Cyberpunk 2077/ { print $1; exit }')
    if [[ -n "${window_id}" ]]; then
      DISPLAY=:0 python3 - "${window_id}" <<'PY' || true
import ctypes as c
import sys

x11 = c.CDLL("libX11.so.6")
x11.XOpenDisplay.restype = c.c_void_p
display = x11.XOpenDisplay(None)
if display:
    window = c.c_ulong(int(sys.argv[1], 16))
    x11.XMapRaised(display, window)
    x11.XRaiseWindow(display, window)
    x11.XSetInputFocus(display, window, 1, 0)
    x11.XSync(display, False)
PY
      exit 0
    fi
    sleep 0.25
  done
) &

# This is intentionally a per-game Gamescope parent, not Wolf's long-running
# Steam compositor.  Cyberpunk submits native PQ, so HDR ITM must stay off.
#
# Steam occasionally consumes the initial `-applaunch` while it is still
# booting, leaving this tile at the SDR REDlauncher rather than starting the
# game. A Steam URI sent after the client exists is reliably forwarded to its
# already-running instance. Send it exactly once: Wine presents the actual
# game's process as `GameThread`, so process-name polling would keep relaunching
# an already-running game.
launch_steam_game() {
  local steam_bin=/usr/games/steam
  # Gamescope supplies this absolute socket only to the command it launches.
  # Using it directly avoids pressure-vessel's `wayland-0` alias being cached
  # as a non-Gamescope display by the WSI layer during Vulkan instance setup.
  if [[ -n "${GAMESCOPE_WAYLAND_DISPLAY:-}" ]]; then
    export WAYLAND_DISPLAY="${GAMESCOPE_WAYLAND_DISPLAY}"
  fi
  "${steam_bin}" &
  local steam_pid=$!

  (
    sleep "${WOLF_STEAM_URI_DELAY:-20}"
    DISPLAY=:0 XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR}" \
      "${steam_bin}" steam://rungameid/1091500 >/dev/null 2>&1 || true
  ) &

  wait "${steam_pid}"
}

exec dbus-run-session -- /usr/games/gamescope \
  -e -b --hdr-enabled --hdr-debug-force-output --hdr-sdr-content-nits "${WOLF_SDR_REFERENCE_WHITE:-203}" --expose-wayland \
  -W "${GAMESCOPE_WIDTH:-1920}" -H "${GAMESCOPE_HEIGHT:-1080}" \
  -w "${GAMESCOPE_WIDTH:-1920}" -h "${GAMESCOPE_HEIGHT:-1080}" \
  -r "${GAMESCOPE_REFRESH:-60}" \
  -- bash -lc "$(declare -f launch_steam_game); launch_steam_game"
