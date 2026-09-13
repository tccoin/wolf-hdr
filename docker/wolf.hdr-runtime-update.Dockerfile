# Keep the proven NVENC/GL runtime intact and replace only the Wolf binary.
# This avoids mixing the current GStreamer 1.28 producer with a later base
# image that no longer ships the matching GL development libraries.
ARG BASE_IMAGE=local/wolf:hdr-native-gl-pq
ARG WOLF_BUILD_IMAGE=local/wolf:hdr-build

FROM ${WOLF_BUILD_IMAGE} AS wolf-build
FROM ${BASE_IMAGE}

COPY --from=wolf-build /wolf/wolf /wolf/wolf
COPY --from=wolf-build /wolf/fake-udev /wolf/fake-udev
