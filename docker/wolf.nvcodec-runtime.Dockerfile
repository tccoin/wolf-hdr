# Assemble the locally-built Wolf binary with the NVIDIA NVENC/CUDA runtime.
# Keep this separate from wolf.vulkan.Dockerfile: its upstream Vulkan image does
# not contain the nvcodec elements required by the configured nvh265enc path.
ARG WOLF_BUILD_IMAGE=local/wolf:hdr-build
ARG NV_CODEC_BASE_IMAGE=local/wolf:hdr-nvcodec
ARG GST_HDR_IMAGE=local/gst-wayland-display:hdr-current
ARG GST_GL_IMAGE=local/gst-wayland-display:hdr-glmemory-debug
ARG GSTREAMER_VERSION=1.28.4
ARG GSTREAMER_COMMIT=b46f881eaa8126eddfd21b5ae5512f8d4ff36255

FROM ${WOLF_BUILD_IMAGE} AS wolf-build
FROM ${GST_HDR_IMAGE} AS gst-hdr-build
FROM ${GST_GL_IMAGE} AS gst-gl-build
FROM scratch AS nvcodec-hdr-patch
COPY docker/nvcodec-rgb10-hdr-vui.patch /nvcodec-rgb10-hdr-vui.patch

FROM ${GST_HDR_IMAGE} AS gst-nvcodec-build
COPY --from=nvcodec-hdr-patch /nvcodec-rgb10-hdr-vui.patch /tmp/nvcodec-rgb10-hdr-vui.patch
ARG GSTREAMER_VERSION
ARG GSTREAMER_COMMIT

# Build nvcodec against the exact GStreamer ABI shipped in /opt/gst.  Mixing
# the old 1.26 nvcodec binary with the 1.28 core serializes the HDR upload and
# encode path at roughly 30 FPS on NVIDIA.
RUN git clone --depth 1 --branch "${GSTREAMER_VERSION}" \
      https://gitlab.freedesktop.org/gstreamer/gstreamer.git /src/gstreamer && \
    test "$(git -C /src/gstreamer rev-parse HEAD)" = "${GSTREAMER_COMMIT}" && \
    git -C /src/gstreamer apply /tmp/nvcodec-rgb10-hdr-vui.patch && \
    meson setup /build /src/gstreamer/subprojects/gst-plugins-bad \
      --prefix=/opt/gst \
      --libdir=lib64 \
      -Dauto_features=disabled \
      -Dgl=enabled \
      -Dnvcodec=enabled \
      -Dnvcodec-cuda-precompile=disabled \
      -Dtests=disabled \
      -Dexamples=disabled \
      -Ddoc=disabled && \
    ninja -C /build sys/nvcodec/libgstnvcodec.so

FROM ${NV_CODEC_BASE_IMAGE} AS nvrtc-libs
RUN dnf install -y --setopt=install_weak_deps=False python3-pip && \
    python3 -m pip install --no-cache-dir --target /nvrtc \
      nvidia-cuda-nvrtc-cu12==12.9.86

FROM ${NV_CODEC_BASE_IMAGE}

# cudaconvertscale is registered only when NVRTC is available at plugin-load
# time. It keeps the RGB->P010 conversion on the GPU, which is required for the
# high-refresh HDR path. Copy only the CUDA 12 NVRTC runtime from NVIDIA's
# wheel; CUDA 11 lacks the CUBIN entry points required by GStreamer 1.28.
COPY --from=nvrtc-libs /nvrtc/nvidia/cuda_nvrtc/lib/ /usr/local/nvidia/lib/
RUN \
    ln -sf "$(basename "$(find /usr/local/nvidia/lib -maxdepth 1 -name 'libnvrtc.so.*' | head -n1)")" \
      /usr/local/nvidia/lib/libnvrtc.so && \
    printf '/usr/local/nvidia/lib\n' >/etc/ld.so.conf.d/nvidia-nvrtc.conf && \
    ldconfig

COPY --from=wolf-build /wolf/wolf /wolf/wolf
COPY --from=wolf-build /wolf/fake-udev /wolf/fake-udev
COPY --from=gst-hdr-build \
    /opt/gst/lib64/gstreamer-1.0/gstreamer-1.0/libgstwaylanddisplaysrc.so \
    /opt/gst/lib64/gstreamer-1.0/gstreamer-1.0/libgstwaylanddisplaysrc.so
# The compositor's HDR bridge and nvh265enc share this exact GL ABI.  Copy only
# the GL library and OpenGL element rather than replacing the base image's core
# GStreamer libraries.
COPY --from=gst-gl-build /opt/gst/lib64/libgstgl-1.0.so.0.2804.0 /opt/gst/lib64/libgstgl-1.0.so.0.2804.0
COPY --from=gst-gl-build /opt/gst/lib64/gstreamer-1.0/libgstopengl.so /opt/gst/lib64/gstreamer-1.0/libgstopengl.so
COPY --from=gst-nvcodec-build \
    /build/sys/nvcodec/libgstnvcodec.so \
    /opt/gst/lib64/gstreamer-1.0/libgstnvcodec.so
# `nvh265enc` GLMemory registration uses libgstcuda's GL interop helpers.
# Keep the pair from one build; combining the new encoder with the base image's
# CUDA library reaches the encode path but aborts while releasing a graphics
# resource.
COPY --from=gst-nvcodec-build \
    /build/gst-libs/gst/cuda/libgstcuda-1.0.so.0.2804.0 \
    /opt/gst/lib64/libgstcuda-1.0.so.0.2804.0

RUN mkdir -p /usr/lib64/gbm && \
    ln -sf /usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so /usr/lib64/gbm/nvidia-drm_gbm.so && \
    ln -sf libgstcuda-1.0.so.0.2804.0 /opt/gst/lib64/libgstcuda-1.0.so.0 && \
    ln -sf libgstgl-1.0.so.0.2804.0 /opt/gst/lib64/libgstgl-1.0.so.0 && \
    ln -sf libgstgl-1.0.so.0.2804.0 /opt/gst/lib64/libgstgl-1.0.so

# GStreamer NVENC's GL interop requires desktop OpenGL 3. Smithay's compositor
# still owns its separate GLES context, so this does not alter rendering there.
ENV GST_GL_API=opengl3 GST_GL_PLATFORM=egl GST_GL_WINDOW=surfaceless \
    WOLF_NATIVE_GL_HDR=1
