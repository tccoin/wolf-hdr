#!/usr/bin/env bash

# Wolf creates the per-lobby compositor asynchronously.  Docker can enter the
# runner before the mounted Wayland socket is listening; Gamescope then exits
# immediately with "Failed to connect to wayland socket".  Do this before
# starting Gamescope for every Steam variant.  A small post-socket delay lets
# the compositor finish announcing its globals.
if [[ -n "${WAYLAND_DISPLAY:-}" && -n "${XDG_RUNTIME_DIR:-}" ]]; then
  wayland_socket="${XDG_RUNTIME_DIR}/${WAYLAND_DISPLAY}"
  deadline=$((SECONDS + 15))
  while (( SECONDS < deadline )) && [[ ! -S "${wayland_socket}" ]]; do
    sleep 0.1
  done
  if [[ ! -S "${wayland_socket}" ]]; then
    echo "Wolf Wayland socket did not become ready: ${wayland_socket}" >&2
    exit 1
  fi
  sleep 1
fi
