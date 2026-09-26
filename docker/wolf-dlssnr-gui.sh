#!/usr/bin/env bash
set -euo pipefail

gui="${HOME}/.local/bin/dlssnr-gui"
if [[ ! -x "$gui" ]]; then
  if command -v kdialog >/dev/null 2>&1; then
    kdialog --error "DLSS5VKLayer GUI was not found at $gui. Install DLSS5VKLayer for this user first."
  else
    printf 'DLSS5VKLayer GUI was not found at %s. Install DLSS5VKLayer for this user first.\n' "$gui" >&2
  fi
  exit 127
fi

exec "$gui" "$@"
