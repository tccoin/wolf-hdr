#!/usr/bin/env bash
# Verify the persistent Wolf-side prerequisites for native DualSense ScePad
# haptics.  This intentionally does not start a game or modify a running
# session; it is safe to use after a rebuild before a user test.
set -Eeuo pipefail

wolf_container="${WOLF_CONTAINER:-wayland_streaming-wolf-1}"
if ! docker inspect --type container "$wolf_container" >/dev/null 2>&1; then
  echo "Wolf container is not available: $wolf_container" >&2
  exit 1
fi

status="$(docker inspect --format '{{.State.Status}}' "$wolf_container")"
if [[ "$status" != running ]]; then
  echo "Wolf container is not running: $wolf_container ($status)" >&2
  exit 1
fi

actual="$(docker exec -u pulse "$wolf_container" sh -eu -c '
  # Docker exec does not inherit the supervisor environment. This is the Wolf
  # documented embedded-Pulse socket. Ignore a stale image-level PULSE_SERVER:
  # that variable is for child applications and is not necessarily valid from
  # docker exec.
  PULSE_SERVER=/tmp/sockets/pulse-socket
  export PULSE_SERVER
  pactl list sinks
')"

require() {
  local value="$1"
  if ! grep -Fq -- "$value" <<<"$actual"; then
    echo "Missing ScePad property: $value" >&2
    exit 1
  fi
}

require 'Name: control_dualsense_audio'
require 'Sample Specification: s16le 4ch 48000Hz'
require 'device.bus = "usb"'
require 'device.vendor.id = "054c"'
require 'device.product.id = "0ce6"'
require 'api.alsa.split.name = "control_dualsense_audio"'

echo "OK: persistent 48 kHz / 4-channel USB DualSense ScePad endpoint is present."
