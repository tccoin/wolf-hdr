#!/usr/bin/env bash
# The base entrypoint sources all hooks into one shell. Keep strict-mode changes
# local so they cannot change behaviour of later upstream hooks.
(
  set -euo pipefail

  # This hook runs as root after the base image has created `retro`, but before
  # the entrypoint drops privileges. Steam Input uses /dev/uinput to expose an
  # XInput pad to Proton games. The device retains the host's numeric group, so
  # mirror that group onto `retro` without weakening the host device mode.
  [[ -c /dev/uinput ]] || exit 0
  id -u retro >/dev/null 2>&1 || exit 0

  uinput_gid=$(stat -c '%g' /dev/uinput)
  uinput_group=$(getent group "$uinput_gid" | cut -d: -f1 || true)
  if [[ -z "$uinput_group" ]]; then
    uinput_group=wolf-uinput
    groupadd -g "$uinput_gid" "$uinput_group"
  fi

  usermod -aG "$uinput_group" retro
)
