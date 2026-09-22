#!/usr/bin/env bash
# Optional runtime check; needs the NVIDIA Container Toolkit, but no display,
# Moonlight connection, personal profile, game, or access to the Docker socket.
set -Eeuo pipefail
image="${1:-wolf:nvcodec-hdr}"
docker run --rm --runtime=nvidia \
  -e NVIDIA_VISIBLE_DEVICES=all -e NVIDIA_DRIVER_CAPABILITIES=all \
  --entrypoint /bin/bash "$image" -ec '
    test -x /wolf/wolf && test -x /wolf/fake-udev
    missing=$(ldd /wolf/wolf | grep "not found" || true)
    if [ -n "$missing" ]; then echo "FAIL Wolf runtime dependencies: $missing" >&2; exit 1; fi
    for element in waylanddisplaysrc interpipesrc interpipesink glupload cudaupload cudaconvertscale nvh265enc h265parse; do
      gst-inspect-1.0 "$element" >/dev/null
      echo "PASS $element"
    done
    gst-inspect-1.0 waylanddisplaysrc | grep -F gl-hdr >/dev/null
    timeout 30 gst-launch-1.0 -q videotestsrc num-buffers=8 ! \
      "video/x-raw,format=RGB10A2_LE,width=1280,height=720,framerate=30/1,colorimetry=bt2100-pq" ! \
      glupload ! cudaupload ! cudaconvertscale ! \
      "video/x-raw(memory:CUDAMemory),format=P010_10LE,colorimetry=bt2100-pq" ! \
      nvh265enc preset=p1 tune=ultra-low-latency ! h265parse ! \
      "video/x-h265,profile=main-10" ! fakesink sync=false
    echo "PASS GL RGB10 -> CUDA P010 -> HEVC Main-10"
  '
