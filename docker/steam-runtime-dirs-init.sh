#!/usr/bin/env bash
# The base entrypoint sources all hooks into one shell. Keep strict-mode changes
# local so they cannot alter the upstream NVIDIA hook that runs after this one.
(
  set -euo pipefail

  # The upstream Steam image initialises ownership of both the X11 socket
  # directory and XDG_RUNTIME_DIR before Gamescope creates either of them.
  # Wolf's per-lobby runtime directory is mounted a moment later, so a fresh
  # host can legitimately be missing /tmp/.X11-unix here.  Make the conventional
  # sticky X11 directory first; this is runtime state only, never game data.
  install -d -m 1777 /tmp/.X11-unix
  install -d -m 1777 "${XDG_RUNTIME_DIR:-/tmp/sockets}"
)
