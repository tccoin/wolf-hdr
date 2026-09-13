#!/usr/bin/env bash
set -euo pipefail

source /opt/gow/steam-wait-wayland.sh

# Wolf adds mouse/keyboard before the virtual gamepad.  Wait for js0 too, so
# Gamescope and Steam enumerate the same controller set from their first scan.
# A keyboard/mouse-only client still continues after the bounded timeout.
deadline=$((SECONDS + 12))
while (( SECONDS < deadline )); do
  if [[ -e /dev/input/mouse0 && -e /dev/input/js0 ]]; then
    break
  fi
  sleep 0.1
done

# This is the general Steam launcher.  It follows the SteamOS layering:
# Gamescope owns HDR output and advertises its capabilities to Steam through
# `-e` (which sets STEAM_GAMESCOPE_HDR_SUPPORTED for the child process).
# Steam/CEF itself remains an SDR client; Gamescope maps it to the HDR output
# at the selected reference white.  Native HDR games inherit the Gamescope WSI
# and DXVK HDR capability only when Moonlight negotiated an HDR transport.
#
# Do *not* globally enable PROTON_ENABLE_WAYLAND here.  That selects Proton's
# experimental Wine Wayland driver for every game and is known to break Steam
# Input/Big Picture controller routing.  A title that specifically needs it
# can opt in in its own Steam launch options after controller testing.
caps="${WOLF_VIDEO_BUFFER_CAPS,,}"
hdr_transport=false
case "$caps" in
  *rgb10*|*p010*|*bt2100*|*mastering-display-info*) hdr_transport=true ;;
esac

common=(
  -e -b --expose-wayland
  -W "${GAMESCOPE_WIDTH:-1920}" -H "${GAMESCOPE_HEIGHT:-1080}"
  -w "${GAMESCOPE_WIDTH:-1920}" -h "${GAMESCOPE_HEIGHT:-1080}"
  -r "${GAMESCOPE_REFRESH:-60}"
)

if "$hdr_transport"; then
  # Gamescope WSI is the HDR compositor contract.  These variables are safe
  # for the Steam client to inherit: the client is not a DXVK/Proton process,
  # while compatible Proton children can expose their HDR toggle.  HDR ITM is
  # deliberately absent; it only applies to SDR input and corrupts native PQ.
  export DXVK_HDR=1
  export ENABLE_GAMESCOPE_WSI=1
  export ENABLE_HDR_WSI=1
  export PROTON_ENABLE_HDR=1
  # SteamOS sessions also explicitly opt the Steam client into Gamescope's
  # colour-managed/virtual-white presentation.  Without these declarations
  # Steam/CEF and ordinary SDR games are treated as wide-gamut HDR content by
  # the virtual connector, producing the visibly over-saturated UI observed
  # in Wolf's HDR Steam lobby.
  export STEAM_GAMESCOPE_COLOR_MANAGED=1
  export STEAM_GAMESCOPE_VIRTUAL_WHITE=1
  unset PROTON_ENABLE_WAYLAND PROTON_ENABLE_NGX_UPDATER DISABLE_HDR_WSI

  exec dbus-run-session -- /usr/games/gamescope "${common[@]}" \
    --hdr-enabled --hdr-debug-force-output --sdr-gamut-wideness 0 \
    --hdr-sdr-content-nits "${WOLF_SDR_REFERENCE_WHITE:-203}" \
    -- /usr/games/steam
fi

# Do not advertise a virtual HDR display to a game when Moonlight requested
# SDR.  This preserves Wolf's native SDR producer/encoder route and prevents
# an SDR client from receiving an HDR/PQ game surface.
unset DXVK_HDR ENABLE_GAMESCOPE_WSI ENABLE_HDR_WSI PROTON_ENABLE_HDR \
  PROTON_ENABLE_WAYLAND PROTON_ENABLE_NGX_UPDATER DISABLE_HDR_WSI \
  STEAM_GAMESCOPE_COLOR_MANAGED STEAM_GAMESCOPE_VIRTUAL_WHITE
exec dbus-run-session -- /usr/games/gamescope "${common[@]}" -- /usr/games/steam
