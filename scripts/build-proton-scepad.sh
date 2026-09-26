#!/usr/bin/env bash
# Build/install Wolf's native-DualSense Wine components from the tracked patch.
#
# This script intentionally requires explicit source, build and Proton paths.
# It never searches for nor edits a user's Steam library on its own.
set -Eeuo pipefail

readonly wine_ref='46b29104e3741fe23bf5e2547196a253aab88c89'
readonly patch_file="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-wine-46b2910.patch"
readonly endpoint_persona_patch="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-endpoint-persona.patch"
readonly dynamic_container_patch="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-dynamic-container.patch"
readonly eager_controller_patch="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-eager-controller.patch"
readonly standard_quad_map_patch="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-standard-quad-map.patch"
readonly force_pulse_route_patch="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)/third_party/wine-scepad/patches/wolf-scepad-force-pulse-route.patch"

wine_source=''
build_x64=''
build_i386=''
proton_dir=''
jobs="${BUILD_JOBS:-16}"
apply_only=false
install_gameinput=false

usage() {
  cat <<'EOF'
Usage:
  bash scripts/build-proton-scepad.sh --wine-source DIR --build-x64 DIR --build-i386 DIR \
    [--proton-dir DIR] [--jobs N] [--apply-only] [--gameinput-v2]

The Wine source must be the public Valve Wine commit 46b2910 and both build
directories must already be configured by the matching Proton build process.
Without --proton-dir, the script only applies the patch and builds artifacts.
--gameinput-v2 is deliberately opt-in: native WineBus is the default input path.
EOF
}

while (($#)); do
  case "$1" in
    --wine-source) wine_source=${2:?}; shift 2 ;;
    --build-x64) build_x64=${2:?}; shift 2 ;;
    --build-i386) build_i386=${2:?}; shift 2 ;;
    --proton-dir) proton_dir=${2:?}; shift 2 ;;
    --jobs) jobs=${2:?}; shift 2 ;;
    --apply-only) apply_only=true; shift ;;
    --gameinput-v2) install_gameinput=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

for required in "$wine_source" "$build_x64" "$build_i386"; do
  [[ -n "$required" && -d "$required" ]] || { usage >&2; exit 2; }
done
for patch in "$patch_file" "$endpoint_persona_patch" "$dynamic_container_patch" "$eager_controller_patch" "$standard_quad_map_patch" "$force_pulse_route_patch"; do
  [[ -f "$patch" ]] || { echo "Missing tracked patch: $patch" >&2; exit 1; }
done

actual_ref="$(git -C "$wine_source" rev-parse HEAD)"
[[ "$actual_ref" == "$wine_ref" ]] || {
  echo "Wine source is $actual_ref; expected $wine_ref" >&2
  exit 1
}

if ! grep -q 'control_dualsense_audio' "$wine_source/dlls/winepulse.drv/pulse.c"; then
  git -C "$wine_source" apply --check "$patch_file"
  git -C "$wine_source" apply "$patch_file"
  echo 'Applied tracked Wolf ScePad patch.'
else
  echo 'Base ScePad patch is already applied.'
fi

if ! grep -q '!strcmp(dev->pulse_name, "control_dualsense_audio")' "$wine_source/dlls/winepulse.drv/pulse.c"; then
  git -C "$wine_source" apply --check "$endpoint_persona_patch"
  git -C "$wine_source" apply "$endpoint_persona_patch"
  echo 'Applied ScePad endpoint-persona patch.'
else
  echo 'ScePad endpoint-persona patch is already applied.'
fi

if ! grep -q 'pulse_get_active_sony_controller(&active_controller)' "$wine_source/dlls/winepulse.drv/pulse.c"; then
  git -C "$wine_source" apply --check "$dynamic_container_patch"
  git -C "$wine_source" apply "$dynamic_container_patch"
  echo 'Applied dynamic ScePad ContainerId patch.'
else
  echo 'Dynamic ScePad ContainerId patch is already applied.'
fi

if ! grep -q 'Audio endpoints are enumerated before the first input report' "$wine_source/dlls/winebus.sys/main.c"; then
  git -C "$wine_source" apply --check "$eager_controller_patch"
  git -C "$wine_source" apply "$eager_controller_patch"
  echo 'Applied eager Sony controller publication patch.'
else
  echo 'Eager Sony controller publication patch is already applied.'
fi

if ! grep -q 'Using standard quad channel map for DualSense haptic stream' "$wine_source/dlls/winepulse.drv/pulse.c"; then
  git -C "$wine_source" apply --check "$standard_quad_map_patch"
  git -C "$wine_source" apply "$standard_quad_map_patch"
  echo 'Applied Wolf ScePad standard-quad-map patch.'
else
  echo 'Wolf ScePad standard-quad-map patch is already applied.'
fi

if ! grep -q 'force_wolf_scepad_pulse_route' "$wine_source/dlls/winepulse.drv/pulse.c"; then
  git -C "$wine_source" apply --check "$force_pulse_route_patch"
  git -C "$wine_source" apply "$force_pulse_route_patch"
  echo 'Applied Wolf ScePad forced-Pulse-route patch.'
else
  echo 'Wolf ScePad forced-Pulse-route patch is already applied.'
fi

if "$apply_only"; then
  exit 0
fi

for build in "$build_x64" "$build_i386"; do
  [[ -f "$build/Makefile" ]] || { echo "Not a configured Wine build: $build" >&2; exit 1; }
done

make -C "$build_x64" -j"$jobs" \
  dlls/winebus.sys/all dlls/winepulse.drv/all dlls/mmdevapi/all server/wineserver
make -C "$build_i386" -j"$jobs" \
  dlls/winebus.sys/all dlls/winepulse.drv/all dlls/mmdevapi/all

if [[ -z "$proton_dir" ]]; then
  echo 'Built ScePad artifacts; --proton-dir was not supplied, so nothing was installed.'
  exit 0
fi
[[ -d "$proton_dir/files/lib/wine" ]] || { echo "Not a Proton directory: $proton_dir" >&2; exit 1; }

install_arch() {
  local build=$1 arch=$2
  local destination="$proton_dir/files/lib/wine"
  install -m 0644 "$build/dlls/winebus.sys/$arch-windows/winebus.sys" "$destination/$arch-windows/winebus.sys"
  install -m 0755 "$build/dlls/winebus.sys/winebus.so" "$destination/$arch-unix/winebus.so"
  install -m 0755 "$build/dlls/winepulse.drv/winepulse.so" "$destination/$arch-unix/winepulse.so"
  install -m 0644 "$build/dlls/mmdevapi/$arch-windows/mmdevapi.dll" "$destination/$arch-windows/mmdevapi.dll"
  install -m 0755 "$build/dlls/mmdevapi/mmdevapi.so" "$destination/$arch-unix/mmdevapi.so"
  if "$install_gameinput"; then
    install -m 0644 "$build/dlls/gameinput/$arch-windows/gameinput.dll" "$destination/$arch-windows/gameinput.dll"
  fi
}

install_arch "$build_x64" x86_64
install_arch "$build_i386" i386
install -m 0755 "$build_x64/server/wineserver" "$proton_dir/files/bin/wineserver"
echo "Installed Wolf ScePad Wine components into: $proton_dir"
