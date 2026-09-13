#!/usr/bin/env bash
set -euo pipefail

# Optional per-game Steam launch wrapper for titles whose HDR toggle is not
# exposed by the normal SteamOS/Gamescope capability handshake.  In Steam,
# set the affected game's launch options to:
#
#   /opt/gow/steam-hdr-game-env.sh %command%
#
# It never enables HDR on an SDR Moonlight session.  It also intentionally
# avoids PROTON_ENABLE_WAYLAND: that is an experimental Wine path which can
# prevent Steam Input from reaching a game.  Add it per-title only if the game
# requires Wayland and its controller input has been verified.
caps="${WOLF_VIDEO_BUFFER_CAPS,,}"
case "$caps" in
  *rgb10*|*p010*|*bt2100*|*mastering-display-info*)
    export DXVK_HDR=1
    export ENABLE_GAMESCOPE_WSI=1
    export ENABLE_HDR_WSI=1
    export PROTON_ENABLE_HDR=1
    unset PROTON_ENABLE_WAYLAND PROTON_ENABLE_NGX_UPDATER DISABLE_HDR_WSI
    ;;
  *)
    unset DXVK_HDR ENABLE_GAMESCOPE_WSI ENABLE_HDR_WSI PROTON_ENABLE_HDR \
      PROTON_ENABLE_WAYLAND PROTON_ENABLE_NGX_UPDATER DISABLE_HDR_WSI
    ;;
esac

exec "$@"
