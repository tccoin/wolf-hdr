#!/usr/bin/env bash
set -euo pipefail

# A normal Wolf stream supplies mouse0 and at least one event node.  A pad is
# added immediately afterwards as js0.  Starting Steam in that small gap makes
# its controller settings cache "no controller" even though Steam Input later
# receives the hot-plug event.  Do not block forever without a gamepad client.
deadline=$((SECONDS + 12))
while (( SECONDS < deadline )); do
  if [[ -e /dev/input/mouse0 && -e /dev/input/js0 ]]; then
    break
  fi
  sleep 0.1
done

exec /opt/gow/startup-default.sh
