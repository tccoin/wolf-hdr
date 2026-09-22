#!/usr/bin/env bash
# Initialise only an empty profile using the bootstrap shipped in the image.
# Existing Steam installations, account data and games are never overwritten.
set -Eeuo pipefail
for candidate in "$HOME/.steam/steam.sh" "$HOME/.steam/steam/steam.sh" "$HOME/.local/share/Steam/steam.sh"; do
  [ ! -x "$candidate" ] || exit 0
done
destination="$HOME/.steam/steam"
mkdir -p "$destination"
if [ -n "$(find "$destination" -mindepth 1 -maxdepth 1 -print -quit)" ]; then
  echo "Steam profile is nonempty but has no bootstrap: $destination; left untouched" >&2
  exit 1
fi
tar -xJf /usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz -C "$destination"
test -x "$destination/steam.sh"
mkdir -p "$HOME/.local/share"
if [ ! -e "$HOME/.local/share/Steam" ] && [ ! -L "$HOME/.local/share/Steam" ]; then
  ln -s "$destination" "$HOME/.local/share/Steam"
fi
