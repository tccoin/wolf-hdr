#!/usr/bin/env bash
set -Eeuo pipefail

usage() {
  cat <<'EOF'
Build Wolf from this checkout, assemble it with the NVIDIA NVENC runtime,
verify the required GStreamer elements, and optionally deploy it.

Usage: scripts/build-hdr-nvcodec.sh [--deploy]

Environment overrides:
  WOLF_BUILD_IMAGE          Intermediate source image (default: local/wolf:hdr-build)
  WOLF_NV_CODEC_BASE_IMAGE  NVENC runtime base (default: local/wolf:hdr-nvcodec)
  WOLF_GST_HDR_BASE_IMAGE   Base used to compile gst-wayland-display
  WOLF_GST_HDR_IMAGE        Intermediate compositor plugin image
  WOLF_STEAM_IMAGE          Steam HDR runner image (default: local/wolf-steam-hdr)
  WOLF_OUTPUT_IMAGE         Verified output image (default: local/wolf:hdr-nvcodec-current)
  WOLF_COMPOSE_DIR          Compose project used by --deploy
EOF
}

deploy=false
case "${1:-}" in
  "") ;;
  --deploy) deploy=true ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${script_dir}/.." && pwd)"
build_image="${WOLF_BUILD_IMAGE:-local/wolf:hdr-build}"
nvcodec_base="${WOLF_NV_CODEC_BASE_IMAGE:-local/wolf:hdr-nvcodec}"
gst_hdr_base="${WOLF_GST_HDR_BASE_IMAGE:-local/gst-wayland-display:vulkan-hdr}"
gst_hdr_image="${WOLF_GST_HDR_IMAGE:-local/gst-wayland-display:hdr-current}"
steam_image="${WOLF_STEAM_IMAGE:-local/wolf-steam-hdr}"
output_image="${WOLF_OUTPUT_IMAGE:-local/wolf:hdr-nvcodec-current}"
candidate_image="${output_image}-candidate"
compose_dir="${WOLF_COMPOSE_DIR:-${repo_dir}/../docker_images/wayland_streaming}"

command -v docker >/dev/null
docker image inspect "${nvcodec_base}" >/dev/null
docker image inspect "${gst_hdr_base}" >/dev/null

cleanup_candidate() {
  docker image rm "${candidate_image}" >/dev/null 2>&1 || true
}
trap cleanup_candidate EXIT

echo "[1/6] Building Wolf source image: ${build_image}"
docker build \
  --progress=plain \
  --file "${repo_dir}/docker/wolf.vulkan.Dockerfile" \
  --tag "${build_image}" \
  "${repo_dir}"

echo "[2/6] Building Steam HDR runner image: ${steam_image}"
docker build \
  --progress=plain \
  --file "${repo_dir}/docker/steam-hdr.Dockerfile" \
  --tag "${steam_image}" \
  "${repo_dir}"

echo "[3/6] Building HDR compositor plugin: ${gst_hdr_image}"
docker build \
  --progress=plain \
  --build-arg "GST_HDR_BASE_IMAGE=${gst_hdr_base}" \
  --file "${repo_dir}/docker/gst-plugin-hdr-refresh.Dockerfile" \
  --tag "${gst_hdr_image}" \
  "${repo_dir}"

echo "[4/6] Assembling NVENC runtime candidate: ${candidate_image}"
docker build \
  --progress=plain \
  --build-arg "WOLF_BUILD_IMAGE=${build_image}" \
  --build-arg "NV_CODEC_BASE_IMAGE=${nvcodec_base}" \
  --build-arg "GST_HDR_IMAGE=${gst_hdr_image}" \
  --file "${repo_dir}/docker/wolf.nvcodec-runtime.Dockerfile" \
  --tag "${candidate_image}" \
  "${repo_dir}"

echo "[5/6] Verifying Wolf binary, compositor plugin, and matching NVIDIA GStreamer elements"
source_hash="$(docker run --rm --entrypoint sha256sum "${build_image}" /wolf/wolf | awk '{print $1}')"
runtime_hash="$(docker run --rm --entrypoint sha256sum "${candidate_image}" /wolf/wolf | awk '{print $1}')"
test -n "${source_hash}"
test "${source_hash}" = "${runtime_hash}"

plugin_path=/opt/gst/lib64/gstreamer-1.0/gstreamer-1.0/libgstwaylanddisplaysrc.so
plugin_hash="$(docker run --rm --entrypoint sha256sum "${gst_hdr_image}" "${plugin_path}" | awk '{print $1}')"
runtime_plugin_hash="$(docker run --rm --entrypoint sha256sum "${candidate_image}" "${plugin_path}" | awk '{print $1}')"
test -n "${plugin_hash}"
test "${plugin_hash}" = "${runtime_plugin_hash}"

docker run --rm \
  --runtime=nvidia \
  --env NVIDIA_VISIBLE_DEVICES=all \
  --env NVIDIA_DRIVER_CAPABILITIES=all \
  --entrypoint /bin/sh \
  "${candidate_image}" \
  -ec 'test -x /wolf/wolf; test "$(gst-inspect-1.0 --version | awk "NR == 1 { print \$3 }")" = "$(gst-inspect-1.0 nvh265enc | awk "/^  Version/ { print \$2; exit }")"; gst-inspect-1.0 vulkandownload >/dev/null; gst-inspect-1.0 cudaupload >/dev/null; gst-inspect-1.0 glupload >/dev/null; gst-inspect-1.0 waylanddisplaysrc | grep -F "gl-hdr" >/dev/null; ldconfig -p | grep -F libnvrtc.so >/dev/null; strings /opt/gst/lib64/gstreamer-1.0/gstreamer-1.0/libgstwaylanddisplaysrc.so | grep -F "HDR GL bridge" >/dev/null'

echo "[5/6] Running SDR and HDR P010-Vulkan-to-NVENC frame-path smoke tests"
for mode in sdr hdr; do
  if [[ "${mode}" == hdr ]]; then
    input_color=bt2100-pq
    output_format=P010_10LE
    output_color=bt2100-pq
    source_hdr=true
    parser=h265parse
    encoder='nvh265enc preset=p1 tune=ultra-low-latency multi-pass=disabled rc-mode=cbr zerolatency=true bitrate=80000'
  else
    input_color=bt709
    output_format=NV12
    output_color=bt709
    source_hdr=false
    parser=h264parse
    encoder='nvh264enc preset=p1 tune=ultra-low-latency multi-pass=disabled rc-mode=cbr zerolatency=true bitrate=80000'
  fi
  docker run --rm --runtime=nvidia --privileged \
    --volume /dev:/dev \
    --env NVIDIA_VISIBLE_DEVICES=all \
    --env NVIDIA_DRIVER_CAPABILITIES=all \
    --env G_DEBUG=fatal-criticals \
    --entrypoint /bin/sh \
    "${candidate_image}" \
    -ec "mkdir -p /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg; timeout 15 gst-launch-1.0 -q waylanddisplaysrc render-node=/dev/dri/renderD128 vulkan=true hdr=${source_hdr} num-buffers=8 ! 'video/x-raw(memory:VulkanImage),format=${output_format},width=3440,height=1440,framerate=165/1,colorimetry=${input_color}' ! vulkandownload ! 'video/x-raw,format=${output_format},colorimetry=${input_color}' ! cudaupload ! 'video/x-raw(memory:CUDAMemory),format=${output_format},width=3440,height=1440,colorimetry=${output_color}' ! ${encoder} ! ${parser} ! fakesink sync=false"
done

echo "[5/6] Running the SDR interpipe bridge used for a Wolf UI -> Steam switch"
docker run --rm --runtime=nvidia --privileged \
  --volume /dev:/dev \
  --env NVIDIA_VISIBLE_DEVICES=all \
  --env NVIDIA_DRIVER_CAPABILITIES=all \
  --env G_DEBUG=fatal-criticals \
  --entrypoint /bin/sh \
  "${candidate_image}" \
  -ec 'mkdir -p /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg; timeout 15 gst-launch-1.0 -q waylanddisplaysrc render-node=/dev/dri/renderD128 num-buffers=8 ! "video/x-raw(memory:CUDAMemory),format=BGRA,width=1920,height=1080,framerate=60/1" ! cudadownload ! videoconvert ! "video/x-raw,format=BGRA,width=1920,height=1080,framerate=60/1" ! cudaupload ! cudaconvertscale add-borders=true ! "video/x-raw(memory:CUDAMemory),format=NV12,width=1920,height=1080,colorimetry=bt709" ! nvh264enc preset=p1 tune=ultra-low-latency multi-pass=disabled rc-mode=cbr zerolatency=true bitrate=30000 ! h264parse ! fakesink sync=false'

echo "[5/6] Running native AB30 PQ RGB-to-P010/Main-10 smoke test"
docker run --rm --runtime=nvidia --privileged \
  --volume /dev:/dev \
  --env NVIDIA_VISIBLE_DEVICES=all \
  --env NVIDIA_DRIVER_CAPABILITIES=all \
  --env G_DEBUG=fatal-criticals \
  --entrypoint /bin/sh \
  "${candidate_image}" \
  -ec 'mkdir -p /tmp/xdg; chmod 700 /tmp/xdg; export XDG_RUNTIME_DIR=/tmp/xdg; timeout 15 gst-launch-1.0 -q waylanddisplaysrc render-node=/dev/dri/renderD128 hdr=true gl-hdr=true num-buffers=8 ! "video/x-raw(memory:GLMemory),format=RGB10A2_LE,colorimetry=bt2100-pq,wolf-hdr-pq=(boolean)true,width=3440,height=1440,framerate=165/1" ! cudaupload ! cudaconvertscale add-borders=true ! "video/x-raw(memory:CUDAMemory),format=P010_10LE,colorimetry=bt2100-pq" ! nvh265enc preset=p1 tune=ultra-low-latency multi-pass=disabled rc-mode=cbr zerolatency=true bitrate=80000 ! h265parse ! "video/x-h265,profile=main-10" ! filesink location=/tmp/wolf-gl-hdr.h265 && gst-launch-1.0 -v filesrc location=/tmp/wolf-gl-hdr.h265 ! h265parse ! fakesink sync=false 2>&1 | grep -F "colorimetry=(string)bt2100-pq" >/dev/null'

docker image tag "${candidate_image}" "${output_image}"
echo "Verified image: ${output_image}"

if "${deploy}"; then
  echo "[6/6] Deploying Wolf from ${output_image}"
  test -f "${compose_dir}/docker-compose.yaml"
  WOLF_IMAGE="${output_image}" docker compose --project-directory "${compose_dir}" up -d --force-recreate wolf
  sleep 3
  docker exec wayland_streaming-wolf-1 /bin/sh -ec \
    'gst-inspect-1.0 nvh265enc >/dev/null; gst-inspect-1.0 cudaconvertscale >/dev/null'
  echo "Deployment verified: wayland_streaming-wolf-1"
else
  echo "[6/6] Build complete (deployment skipped; pass --deploy to restart Wolf)"
fi
