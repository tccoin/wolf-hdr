#!/usr/bin/env bash
set -euo pipefail

# Keep the regular Steam tile and the HDR Steam tile on exactly the same
# transport-aware implementation.  Two copies had diverged: one unconditionally
# forced HDR, while the other globally enabled Wine Wayland.  The canonical
# launcher now selects HDR only when the connected Moonlight client requested it.
exec /opt/gow/steam-hdr-startup.sh "$@"
