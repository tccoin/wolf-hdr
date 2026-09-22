#!/usr/bin/env bash
# Isolated end-to-end test. Never mounts a user profile or starts Steam/games.
set -Eeuo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
wolf_image="${1:-wolf:nvcodec-hdr}"
kde_image="${2:-wolf-kde:hdr}"
render_node="${WOLF_TEST_RENDER_NODE:?Set WOLF_TEST_RENDER_NODE to the NVIDIA render node}"
source_depth="${WOLF_PQ_SOURCE_DEPTH:-10}"
test_dir="$(mktemp -d /tmp/wolf-hdr-regression.XXXXXX)"
# The image's desktop uid need not equal the invoking host uid. Permit
# traversal to the explicitly mounted, non-sensitive test artifacts only.
chmod 0711 "$test_dir"
suffix="${test_dir##*.}"
producer="wolf-hdr-probe-$suffix"
desktop="wolf-kde-probe-$suffix"
cleanup() {
  docker logs "$producer" >"$test_dir/producer.log" 2>&1 || true
  docker logs "$desktop" >"$test_dir/desktop.log" 2>&1 || true
  docker stop -t 3 "$desktop" "$producer" >/dev/null 2>&1 || true
  docker rm "$desktop" "$producer" >/dev/null 2>&1 || true
  echo "Test evidence retained: $test_dir"
}
trap cleanup EXIT
extra=()
if [ -n "${WOLF_BUILDER:-}" ]; then extra+=(--builder "$WOLF_BUILDER"); fi
if [ -n "${WOLF_PQ_CLIENT:-}" ]; then
  # Developer iteration: reuse an explicitly selected source-built probe.
  mkdir "$test_dir/client"
  install -m 0755 "$WOLF_PQ_CLIENT" "$test_dir/client/hdr-pq-patches"
else
  docker buildx build "${extra[@]}" --target hdr-probe \
    --build-arg "BUILD_JOBS=${BUILD_JOBS:-4}" \
    --output "type=local,dest=$test_dir/client" \
    -f "$repo_dir/docker/kde-hdr.Dockerfile" "$repo_dir"
fi
docker run -d --name "$producer" --runtime=nvidia --device /dev/dri \
  -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=all \
  -e XDG_RUNTIME_DIR=/probe -e WOLF_HDR_CM=1 -e WOLF_SDR_REFERENCE_WHITE=100 \
  -e WOLF_HDR_MASTERING=35400:14600:8500:39850:6550:2300:15635:16450:10000000:1 \
  -e WOLF_HDR_CLL=1000:400 -e "PROBE_RENDER_NODE=$render_node" \
  -v "$test_dir:/probe" --entrypoint bash "$wolf_image" -ec '
    exec gst-launch-1.0 -e waylanddisplaysrc render-node="$PROBE_RENDER_NODE" \
      gl-hdr=true hdr=true num-buffers=1800 ! \
      "video/x-raw(memory:GLMemory),format=RGB10A2_LE,width=1280,height=720,framerate=10/1" ! tee name=t \
      t. ! queue ! gldownload ! "video/x-raw,format=RGB10A2_LE" ! filesink location=/probe/frames.rgb10 \
      t. ! queue ! cudaupload ! "video/x-raw(memory:CUDAMemory),format=RGB10A2_LE" ! \
      cudaconvertscale ! "video/x-raw(memory:CUDAMemory),format=P010_10LE,colorimetry=bt2100-pq" ! \
      nvh265enc preset=p1 tune=ultra-low-latency ! h265parse ! \
      "video/x-h265,stream-format=byte-stream,profile=main-10" ! filesink location=/probe/output.h265
  ' >/dev/null
socket=""
for _ in {1..100}; do
  socket="$(find "$test_dir" -maxdepth 1 -type s -name 'wayland-*' -printf '%f\n' -quit)"
  [ -z "$socket" ] || break
  sleep .1
done
test -n "$socket"
# Wolf normally grants its app user access to this socket. The standalone
# GStreamer source has no runner manager, so do that for this private socket.
docker exec "$producer" chmod 777 "/probe/$socket"
docker run -d --name "$desktop" --runtime=nvidia --user root --device /dev/dri \
  --cap-add SYS_ADMIN --cap-add SYS_NICE --cap-add SYS_PTRACE \
  --cap-add NET_RAW --cap-add MKNOD --cap-add NET_ADMIN \
  --security-opt seccomp=unconfined --security-opt apparmor=unconfined \
  -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=all \
  -e XDG_RUNTIME_DIR=/probe -e "WAYLAND_DISPLAY=$socket" -e HOME=/home/retro \
  -e WOLF_SESSION_ID=regression -e WOLF_KDE_ENABLE_HDR=1 \
  -e WOLF_KDE_HDR_PEAK_NITS=550 -e WOLF_KDE_START_STEAM=0 \
  -e GAMESCOPE_WIDTH=1280 -e GAMESCOPE_HEIGHT=720 -e GAMESCOPE_REFRESH=10 \
  -v "$test_dir:/probe" "$kde_image" /usr/local/bin/wolf-selkies-kwin-entrypoint >/dev/null
plasma_pid=""
for _ in {1..150}; do
  plasma_pid="$(docker exec "$desktop" pgrep -o -x plasmashell || true)"
  [ -z "$plasma_pid" ] || break
  sleep .2
done
test -n "$plasma_pid"
docker exec -i -u ubuntu "$desktop" python3 - "$plasma_pid" \
  < "$repo_dir/tests/platforms/linux/check-chrome-sandbox.py"
frame_count() { echo "$(( $(stat -c %s "$test_dir/frames.rgb10") / (1280 * 720 * 4) ))"; }
start="$(frame_count)"
docker exec -u ubuntu -e "WOLF_PQ_SOURCE_DEPTH=$source_depth" -e XDG_RUNTIME_DIR=/tmp/wolf-selkies-kwin-regression \
  -e WAYLAND_DISPLAY=wayland-kde "$desktop" /probe/client/hdr-pq-patches 550 100 \
  >"$test_dir/direct.log" 2>&1
python3 "$repo_dir/tests/platforms/linux/check-hdr-pq-capture.py" "$test_dir/frames.rgb10" \
  --start-frame "$start" --end-frame "$(frame_count)" --source-depth "$source_depth"
start="$(frame_count)"
gamescope_status=0
gamescope_command=(/usr/games/gamescope)
if [ "${WOLF_TEST_GDB:-0}" = 1 ]; then
  gamescope_command=(gdb --return-child-result -batch -ex 'set pagination off'
    -ex 'handle SIGINT nostop noprint pass' -ex run
    -ex 'thread apply all bt 15' --args /usr/games/gamescope)
fi
docker exec -u ubuntu -e "WOLF_PQ_SOURCE_DEPTH=$source_depth" -e XDG_RUNTIME_DIR=/tmp/wolf-selkies-kwin-regression \
  -e WAYLAND_DISPLAY=wayland-kde -e WAYLAND_DEBUG=client "$desktop" \
  timeout --signal=TERM --kill-after=10s 60s "${gamescope_command[@]}" \
  --backend wayland -e -f --virtual-connector-strategy SingleApplication --hdr-enabled \
  --hdr-sdr-content-nits 100 \
  -W 1280 -H 720 -w 1280 -h 720 -r 10 -- /probe/client/hdr-pq-patches \
  >"$test_dir/gamescope.log" 2>&1 || gamescope_status=$?
echo "Gamescope exit status: $gamescope_status"
python3 "$repo_dir/tests/platforms/linux/check-hdr-pq-capture.py" "$test_dir/frames.rgb10" \
  --start-frame "$start" --end-frame "$(frame_count)" --source-depth "$source_depth"
docker exec "$desktop" ffprobe -v error -read_intervals '%+#1' -select_streams v:0 \
  -show_entries stream=profile,pix_fmt,color_space,color_transfer,color_primaries \
  -of json /probe/output.h265 >"$test_dir/encoded.json"
python3 - "$test_dir/encoded.json" <<'PY'
import json, sys
with open(sys.argv[1]) as source:
    stream = json.load(source)['streams'][0]
expected = dict(profile='Main 10', pix_fmt='yuv420p10le', color_space='bt2020nc',
                color_transfer='smpte2084', color_primaries='bt2020')
assert stream == expected, stream
print('PASS encoded HEVC HDR signaling:', stream)
PY
test "$gamescope_status" -eq 0
echo 'PASS Chrome + direct KWin PQ + Gamescope/KWin PQ + clean Gamescope exit + NVENC HDR10'
