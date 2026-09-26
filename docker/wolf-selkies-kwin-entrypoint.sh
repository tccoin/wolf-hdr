#!/usr/bin/env bash
set -Eeuo pipefail

# Preserve the outer Wolf socket before privilege dropping. `runuser` is free
# to change XDG_RUNTIME_DIR, but the nested compositor must never derive its
# parent path from that post-drop value.
parent_runtime="${WOLF_PARENT_RUNTIME:-${XDG_RUNTIME_DIR:-/tmp/sockets}}"
parent_display="${WOLF_PARENT_WAYLAND_DISPLAY:-${WAYLAND_DISPLAY:-wayland-0}}"
export XDG_RUNTIME_DIR="/tmp/wolf-selkies-kwin-${WOLF_SESSION_ID:-session}"
mkdir -p "$XDG_RUNTIME_DIR" /tmp/.X11-unix
chmod 0700 "$XDG_RUNTIME_DIR"
chmod 1777 /tmp/.X11-unix
ln -snf "$parent_runtime/$parent_display" "$XDG_RUNTIME_DIR/$parent_display"
export WAYLAND_DISPLAY="$parent_display"

export HOME="${HOME:-/home/ubuntu}"
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE
export XDG_SESSION_TYPE=wayland
export KDE_FULL_SESSION=true
export KDE_SESSION_VERSION=6
export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1
export KWIN_OPENGL_INTERFACE=egl
export KWIN_DISABLE_VULKAN=1
export KWIN_FORCE_SW_CURSOR=1
export KWIN_WAYLAND_HDR_PEAK_NITS="${WOLF_KDE_HDR_PEAK_NITS:-0}"
# Wolf emits 3 surface units for 120 wheel units. KWayland's nested-pointer
# wrapper drops the discrete/source events; restore their known wire units.
export KWIN_WAYLAND_SCROLL_V120_PER_UNIT=40

# Wolf creates a fresh runner-state directory as root.  On a first launch the
# image's default `ubuntu` user then cannot even create the KWin/Plasma logs,
# which makes the tile exit before the compositor comes up.  Initialise the
# mounted home directory once as root and immediately drop privileges again;
# KWin, Plasma and Steam always run as ubuntu.
if [ "$(id -u)" = "0" ] && [ "${WOLF_KWIN_ROOT_INIT:-0}" != "1" ]; then
  # Steam runs as the unprivileged KDE user.  It needs uinput write access to
  # rebuild its virtual XInput pad when Moonlight disconnects and reconnects a
  # controller while a game is running.  The host device's group is dynamic,
  # so use its numeric owner instead of assuming the image's `input` group.
  if [ -c /dev/uinput ]; then
    uinput_gid="$(stat -c '%g' /dev/uinput)"
    uinput_group="$(getent group "$uinput_gid" | cut -d: -f1 || true)"
    if [ -n "$uinput_group" ]; then
      usermod -a -G "$uinput_group" ubuntu
    else
      groupadd -g "$uinput_gid" "wolf-uinput-$uinput_gid"
      usermod -a -G "wolf-uinput-$uinput_gid" ubuntu
    fi
  fi

  # Docker passes the host DRM render node through with its *numeric* owner
  # and group.  On this host it is root:root (0660), whereas the image's
  # desktop user is only in the image-local `render` group (GID 992).  Do not
  # assume a distro-specific render GID: grant ubuntu membership of whatever
  # group owns the actual mounted node before KWin/Chrome are started.
  for drm_node in /dev/dri/renderD*; do
    [ -e "$drm_node" ] || continue
    drm_gid="$(stat -c '%g' "$drm_node")"
    drm_group="$(getent group "$drm_gid" | cut -d: -f1 || true)"
    if [ -n "$drm_group" ]; then
      usermod -a -G "$drm_group" ubuntu
    else
      groupadd -g "$drm_gid" "wolf-drm-$drm_gid"
      usermod -a -G "wolf-drm-$drm_gid" ubuntu
    fi
  done
  mkdir -p /home/retro
  chown ubuntu:ubuntu /home/retro
  # Docker cannot bind both /home/retro and its .steam child.  The shared
  # Steam profile is mounted at a sibling path, then linked into the KDE home
  # before dropping privileges.  This keeps the existing Steam login/library
  # intact while allowing the desktop/Chrome profile to persist independently.
  if [ -d /var/lib/wolf-shared-steam ]; then
    # Selkies creates an empty placeholder on first boot. It contains no
    # Steam state, so remove only that verified-empty directory before making
    # the shared-profile link. A non-empty path is deliberately preserved.
    if [ -d /home/retro/.steam ] && [ ! -L /home/retro/.steam ] && \
       [ -z "$(find /home/retro/.steam -mindepth 1 -maxdepth 1 -print -quit)" ]; then
      rmdir /home/retro/.steam
    fi
    if [ ! -e /home/retro/.steam ]; then
      ln -s /var/lib/wolf-shared-steam /home/retro/.steam
    fi
  fi
  runtime_dir="/tmp/wolf-selkies-kwin-${WOLF_SESSION_ID:-session}"
  mkdir -p "$runtime_dir" /tmp/.X11-unix
  # The first, root-owned pass through this entrypoint may already have made
  # the parent-socket symlink. Remove it before dropping privileges so the
  # ubuntu pass can create the final link itself.
  rm -f "$runtime_dir/$parent_display"
  chown ubuntu:ubuntu "$runtime_dir"
  chmod 0700 "$runtime_dir"
  chmod 1777 /tmp/.X11-unix
  exec runuser -u ubuntu -- env \
    WOLF_KWIN_ROOT_INIT=1 \
    HOME=/home/retro \
    WOLF_PARENT_RUNTIME="$parent_runtime" \
    WOLF_PARENT_WAYLAND_DISPLAY="$parent_display" \
    "$0" "$@"
fi

export HOME=/home/retro

# A persistent browser/Steam home must not be opened by two runner containers.
# Keep this lock for the session lifetime, including surviving child processes.
exec 7>"$HOME/.wolf-kde-session.lock"
if ! flock -n 7; then
  echo "[wolf-selkies-kwin] another KDE session owns this profile" >&2
  exit 1
fi
# Chrome's SingletonLock embeds the previous container hostname. After that
# managed session ends the hostname is obsolete, but Chrome calls it another
# computer. Only migrate locks owned by our recorded, now-unlocked session;
# unknown/external profile owners are deliberately left for manual inspection.
previous_host=""
owner_file="$HOME/.wolf-kde-profile-owner"
if [ -f "$owner_file" ]; then read -r previous_host < "$owner_file" || true; fi
chrome_profile="$HOME/.config/google-chrome"
if [[ "$previous_host" =~ ^[a-zA-Z0-9][a-zA-Z0-9._-]{0,63}$ ]] && \
   [ -L "$chrome_profile/SingletonLock" ] && \
   [[ "$(readlink "$chrome_profile/SingletonLock")" == "$previous_host-"* ]]; then
  stale_dir="$(mktemp -d "$chrome_profile/wolf-stale-locks.XXXXXX")"
  for singleton in SingletonLock SingletonSocket SingletonCookie; do
    if [ -L "$chrome_profile/$singleton" ]; then
      mv -- "$chrome_profile/$singleton" "$stale_dir/"
    fi
  done
  echo "[wolf-selkies-kwin] archived stale managed Chrome locks: $stale_dir"
fi
hostname > "$owner_file"

# The home is persistent and masks image Desktop files; refresh both managed
# launchers on each session, without changing the shared Steam profile.
mkdir -p "$HOME/Desktop"
install -m 0755 /usr/local/share/wolf/steam.desktop "$HOME/Desktop/steam.desktop"
install -m 0755 /usr/local/share/wolf/steam-big-picture.desktop "$HOME/Desktop/steam-big-picture.desktop"
install -m 0755 /usr/local/share/wolf/heroic.desktop "$HOME/Desktop/heroic.desktop"
install -m 0755 /usr/local/share/wolf/dualsense-trigger-test.desktop "$HOME/Desktop/dualsense-trigger-test.desktop"
install -m 0755 /usr/local/share/wolf/dlssnr.desktop "$HOME/Desktop/dlssnr.desktop"
install -m 0755 /usr/local/share/wolf/return-to-wolf-ui.desktop "$HOME/Desktop/return-to-wolf-ui.desktop"
# Retire only the old Wolf-generated shortcut; preserve user-customized files.
if [ -f "$HOME/Desktop/heroic-hdr.desktop" ] && \
   grep -qx 'Exec=/usr/local/bin/heroic --wolf-hdr' "$HOME/Desktop/heroic-hdr.desktop"; then
  mkdir -p "$HOME/.local/share/wolf/retired-shortcuts"
  mv -n "$HOME/Desktop/heroic-hdr.desktop" "$HOME/.local/share/wolf/retired-shortcuts/"
fi

# Wolf removes one-shot runner containers after an early exit.  Keep the
# compositor and Plasma startup trace in the existing runner-state mount so a
# failed launch remains diagnosable after the container is gone.
mkdir -p /home/retro
exec > >(tee /home/retro/wolf-selkies-runtime.log) 2>&1

exec dbus-run-session -- bash -lc '
  set -Eeuo pipefail
  # Wolf injects the negotiated Moonlight rate as GAMESCOPE_REFRESH (Hz).
  # The KWin Wayland backend otherwise starts at its upstream 60Hz fallback
  # until a parent presentation-time event arrives.
  if [ "${GAMESCOPE_REFRESH:-}" -gt 0 ] 2>/dev/null; then
    export KWIN_WAYLAND_REFRESH="$GAMESCOPE_REFRESH"
  fi
  if [ "${WOLF_KDE_WAYLAND_DEBUG:-0}" = "1" ]; then
    # Temporary protocol tracing switch for nested-output diagnostics.  It is
    # opt-in so normal KDE sessions never pay the log volume cost.
    export WAYLAND_DEBUG=client
  fi
  kwin_log=/home/retro/wolf-selkies-kwin.log
  # This compositor is the streamed desktop: it must own Alt+Tab / Meta+D.
  # Disabling global shortcuts here traps focus in fullscreen Gamescope.
  kwin_wayland --xwayland --no-lockscreen \
    --wayland-display="$WAYLAND_DISPLAY" --socket=wayland-kde \
    >"$kwin_log" 2>&1 &
  kwin_pid=$!
  for _ in $(seq 1 100); do
    if [ -S "$XDG_RUNTIME_DIR/wayland-kde" ]; then break; fi
    if ! kill -0 "$kwin_pid" 2>/dev/null; then
      cat "$kwin_log" >&2 || true
      exit 1
    fi
    sleep .1
  done
  test -S "$XDG_RUNTIME_DIR/wayland-kde" || { cat "$kwin_log" >&2; exit 1; }
  export WAYLAND_DISPLAY=wayland-kde
  # The outer Wolf compositor advertised BT.2020 + PQ and patched KWin has
  # exposed those capabilities to KScreen. Enable both output modes before
  # Plasma starts so a new runner is HDR from its first rendered frame.
  if [ "${WOLF_KDE_ENABLE_HDR:-0}" = "1" ]; then
    # Do not set maxBrightnessOverride here. KScreen owns that persisted user
    # calibration and must restore exactly what the user saved. The separate
    # WOLF_KDE_HDR_PEAK_NITS value only supplies the uncalibrated KWin base peak;
    # WOLF_HDR_PEAK_NITS describes the game/stream display contract.
    if ! kscreen-doctor output.WL-0.hdr.enable output.WL-0.wcg.enable \
        >>"$kwin_log" 2>&1; then
      echo "[wolf-selkies-kwin] failed to enable nested HDR/WCG" >&2
    fi
  fi
  # Steam remains an X11 client in this image. The KWin --xwayland option creates X0,
  # but does not export DISPLAY to sibling processes.
  for _ in $(seq 1 50); do
    [ -S /tmp/.X11-unix/X0 ] && break
    sleep .1
  done
  export DISPLAY=:0
  # The Selkies base image contains both X11 and Wayland Qt backends.  Without
  # pinning the nested compositor xdg-shell integration, Qt probes wl-shell,
  # falls back to X11, and plasmashell exits before it can draw its first frame.
  export QT_QPA_PLATFORM=wayland
  export QT_WAYLAND_SHELL_INTEGRATION=xdg-shell
  echo "[wolf-selkies-kwin] nested KWin ready on $XDG_RUNTIME_DIR/$WAYLAND_DISPLAY"
  plasma_log=/home/retro/wolf-selkies-plasma.log
  echo "[wolf-selkies-kwin] starting plasmashell" >&2
  start_plasma() {
    plasmashell >>"$plasma_log" 2>&1 &
    plasma_pid=$!
  }
  start_plasma
  steam_pid=""
  if [ "${WOLF_KDE_START_STEAM:-0}" = "1" ]; then
    # ~/.steam is a bind mount of the existing WolfSteam persistent profile.
    # It is deliberately never copied or removed here: ending this KDE runner
    # only terminates its Steam process, leaving the shared Steam library,
    # login and controller state intact for the normal Steam HDR tiles.
    steam_log=/home/retro/wolf-selkies-steam.log
    steam_mode="${WOLF_KDE_STEAM_MODE:-big-picture}"
    echo "[wolf-selkies-kwin] starting existing Steam profile ($steam_mode)" >&2
    mkdir -p /home/retro/.local/share
    if [ ! -e /home/retro/.local/share/Steam ]; then
      ln -s /home/retro/.steam/steam /home/retro/.local/share/Steam
    fi
    # The same wrapper is installed at the path used by the desktop shortcut.
    # It owns HDR/controller environment and can reopen Steam after quitting.
    if [ "$steam_mode" = "big-picture" ]; then
      /usr/bin/steam --wolf-big-picture >"$steam_log" 2>&1 &
    else
      /usr/bin/steam --wolf-desktop >"$steam_log" 2>&1 &
    fi
    steam_pid=$!
  fi
  # Plasma occasionally exits during portal/X11 initialisation in a nested
  # session.  It is not the session owner: keep KWin and Steam alive and
  # restart Plasma instead of letting `set -e` tear down the Wolf runner.
  while kill -0 "$kwin_pid" 2>/dev/null; do
    plasma_status=0
    wait "$plasma_pid" || plasma_status=$?
    echo "[wolf-selkies-kwin] plasmashell exited with status $plasma_status; restarting" >&2
    sleep 1
    kill -0 "$kwin_pid" 2>/dev/null || break
    start_plasma
  done
  echo "[wolf-selkies-kwin] nested KWin exited; stopping child processes" >&2
  if [ -n "$steam_pid" ]; then
    kill "$steam_pid" 2>/dev/null || true
    wait "$steam_pid" 2>/dev/null || true
  fi
  exit 1
'
