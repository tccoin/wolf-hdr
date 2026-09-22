# NVIDIA HDR image built entirely from public sources and this repository.
# Build context: repository root. No workstation image or compiled .so input.
ARG BASE_IMAGE=ghcr.io/games-on-whales/base-app:fedora@sha256:d29171bb180d01ea2cb45993d974eca9265da37b6745c67f75cca62a4d193a9d
FROM ${BASE_IMAGE} AS runtime-base
USER root
RUN dnf install -y --allowerasing \
      ca-certificates openssl-libs libicu libevdev systemd-libs libcurl libdrm \
      pciutils-libs libunwind libwayland-server libinput libxkbcommon mesa-libgbm \
      libglvnd mesa-libGL mesa-libEGL mesa-libGLES vulkan-loader \
      glib2 opus orc libX11 libXext libXfixes libXdamage libxcb \
      pulseaudio pulseaudio-utils supervisor python3-pip \
 && dnf clean all
ARG NVRTC_VERSION=12.9.86
RUN python3 -m pip install --no-cache-dir --target /nvrtc nvidia-cuda-nvrtc-cu12==${NVRTC_VERSION} \
 && mkdir -p /usr/local/nvidia/lib /usr/lib64/gbm \
 && cp -a /nvrtc/nvidia/cuda_nvrtc/lib/. /usr/local/nvidia/lib/ \
 && ln -s libnvrtc.so.12 /usr/local/nvidia/lib/libnvrtc.so \
 && ln -s /usr/lib/x86_64-linux-gnu/gbm/nvidia-drm_gbm.so /usr/lib64/gbm/nvidia-drm_gbm.so \
 && printf '%s\n' /usr/local/nvidia/lib /opt/gst/lib64 > /etc/ld.so.conf.d/wolf-hdr.conf \
 && rm -rf /nvrtc

FROM runtime-base AS toolchain
ARG BUILD_JOBS=4
RUN dnf install -y dnf-plugins-core 'dnf-command(builddep)' \
 && dnf builddep -y gstreamer1 gstreamer1-plugins-base gstreamer1-plugins-good gstreamer1-plugins-bad-free \
 && dnf install -y \
      git curl gcc gcc-c++ make cmake pkgconf-pkg-config ninja-build nasm flex bison meson \
      glib2-devel libdrm-devel mesa-libgbm-devel systemd-devel wayland-devel wayland-protocols-devel \
      libxkbcommon-devel libX11-devel libinput-devel openssl-devel clang clang-devel llvm-devel \
      libffi-devel expat-devel vulkan-headers vulkan-loader-devel libglvnd-devel mesa-libEGL-devel \
      opus-devel pulseaudio-libs-devel libevdev-devel libcurl-devel pciutils-devel libunwind-devel \
      glibc-static libstdc++-static ccache \
 && dnf clean all
ARG GSTREAMER_VERSION=1.28.4
ARG GSTREAMER_COMMIT=b46f881eaa8126eddfd21b5ae5512f8d4ff36255
COPY docker/nvcodec-rgb10-hdr-vui.patch /tmp/nvcodec.patch
COPY third_party/gst-wayland-display/patches/vkh264enc-dpb-pool-in-new-sequence.patch /tmp/dpb.patch
COPY third_party/gst-wayland-display/patches/vulkanh265enc.patch /tmp/h265.patch
RUN git clone --depth=1 --branch ${GSTREAMER_VERSION} https://gitlab.freedesktop.org/gstreamer/gstreamer.git /src/gstreamer \
 && test "$(git -C /src/gstreamer rev-parse HEAD)" = "${GSTREAMER_COMMIT}" \
 && cd /src/gstreamer \
 && git apply /tmp/nvcodec.patch /tmp/dpb.patch /tmp/h265.patch \
 && for d in subprojects/*/docs/meson.build docs/meson.build; do \
      [ -f "$d" ] || continue; \
      { printf "if not get_option('doc').allowed()\n  subdir_done()\nendif\n"; cat "$d"; } > "$d.tmp"; \
      mv "$d.tmp" "$d"; \
    done \
 && meson setup build --prefix=/opt/gst --libdir=lib64 \
      -Dauto_features=disabled -Dbase=enabled -Dbad=enabled -Dgood=enabled -Dtools=enabled \
      -Dugly=disabled -Dlibav=disabled -Dges=disabled -Drtsp_server=disabled \
      -Ddevtools=disabled -Dpython=disabled -Dsharp=disabled -Drs=disabled \
      -Dgst-plugins-base:app=enabled -Dgst-plugins-base:typefind=enabled \
      -Dgst-plugins-base:videotestsrc=enabled -Dgst-plugins-base:videoconvertscale=enabled \
      -Dgst-plugins-base:playback=enabled -Dgst-plugins-base:drm=enabled \
      -Dgst-plugins-base:audioconvert=enabled -Dgst-plugins-base:audioresample=enabled \
      -Dgst-plugins-base:audiorate=enabled -Dgst-plugins-base:opus=enabled -Dgst-plugins-base:volume=enabled \
      -Dgst-plugins-base:gl=enabled -Dgst-plugins-base:gl_winsys=egl,gbm,surfaceless \
      -Dgst-plugins-good:pulse=enabled -Dgst-plugins-bad:gl=enabled \
      -Dgst-plugins-bad:vulkan=enabled -Dgst-plugins-bad:vulkan-video=enabled \
      -Dgst-plugins-bad:nvcodec=enabled -Dgst-plugins-bad:nvcodec-cuda-precompile=disabled \
      -Dgst-plugins-bad:videoparsers=enabled -Dgst-plugins-bad:wayland=enabled \
      -Dorc=disabled -Ddoc=disabled -Dgtk_doc=disabled -Dintrospection=disabled \
      -Dexamples=disabled -Dtests=disabled -Dnls=disabled -Dgst-examples=disabled \
 && meson compile -C build -j${BUILD_JOBS} && meson install -C build
ENV PKG_CONFIG_PATH=/opt/gst/lib64/pkgconfig \
    LD_LIBRARY_PATH=/opt/gst/lib64:/usr/local/nvidia/lib \
    GST_PLUGIN_PATH=/opt/gst/lib64/gstreamer-1.0 \
    PATH=/opt/gst/bin:/root/.cargo/bin:/usr/local/bin:/usr/bin:/bin \
    LIBCLANG_PATH=/usr/lib64
ARG INTERPIPE_REF=0c454917cb3cf4f2b2f2729f5f72a4dee072c7c9
RUN git clone https://github.com/games-on-whales/gst-interpipe.git /src/gst-interpipe \
 && cd /src/gst-interpipe && git checkout --detach ${INTERPIPE_REF} \
 && meson setup build --prefix=/opt/gst --libdir=lib64 -Denable-gtk-doc=false \
 && meson compile -C build -j${BUILD_JOBS} && meson install -C build
ARG RUST_VERSION=1.96.0
ENV CARGO_HOME=/root/.cargo RUSTUP_HOME=/root/.rustup
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | \
      sh -s -- -y --default-toolchain ${RUST_VERSION} --profile minimal \
 && cargo install cargo-c --version 0.10.23 --locked

FROM toolchain AS compositor-build
COPY third_party/gst-wayland-display/ /src/gst-wayland-display/
WORKDIR /src/gst-wayland-display
RUN cargo cinstall --locked --release -p gst-plugin-wayland-display --features cuda,gl-hdr \
      --prefix=/opt/gst --libdir=/opt/gst/lib64/gstreamer-1.0 \
      --pkgconfigdir=/opt/gst/lib64/pkgconfig

FROM compositor-build AS wolf-builder
ARG BUILD_JOBS=4
COPY CMakeLists.txt /wolf/CMakeLists.txt
COPY cmake/ /wolf/cmake/
COPY src/ /wolf/src/
WORKDIR /wolf
RUN cmake -S . -B /build/wolf -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_STANDARD=20 -DBUILD_SHARED_LIBS=OFF -DBoost_USE_STATIC_LIBS=ON \
      -DBUILD_TESTING=OFF -DBUILD_FAKE_UDEV_CLI=ON \
 && cmake --build /build/wolf --target wolf fake-udev --parallel ${BUILD_JOBS}

FROM runtime-base AS runner
COPY --from=compositor-build /opt/gst/ /opt/gst/
COPY --from=wolf-builder /build/wolf/src/moonlight-server/wolf /wolf/wolf
COPY --from=wolf-builder /build/wolf/src/fake-udev/fake-udev /wolf/fake-udev
COPY docker/supervisord.conf /etc/supervisord.conf
COPY --chmod=0755 docker/startup.sh /opt/gow/startup-app.sh
RUN dnf install -y libatomic && dnf clean all && ldconfig \
 && ! ldd /wolf/wolf | grep 'not found'
WORKDIR /wolf
ENV PATH=/opt/gst/bin:/usr/local/bin:/usr/bin:/bin \
    LD_LIBRARY_PATH=/opt/gst/lib64:/usr/local/nvidia/lib \
    GST_PLUGIN_PATH=/opt/gst/lib64/gstreamer-1.0 \
    GST_GL_API=opengl3 GST_GL_PLATFORM=egl GST_GL_WINDOW=surfaceless \
    WOLF_USE_ZERO_COPY=TRUE WOLF_NATIVE_GL_HDR=1 WOLF_KDE_NATIVE_HDR=1 WOLF_HDR_CM=1 \
    WOLF_SDR_REFERENCE_WHITE=100 WOLF_LOG_LEVEL=INFO \
    WOLF_CFG_FOLDER=/etc/wolf/cfg WOLF_CFG_FILE=/etc/wolf/cfg/config.toml \
    WOLF_PRIVATE_KEY_FILE=/etc/wolf/cfg/key.pem WOLF_PRIVATE_CERT_FILE=/etc/wolf/cfg/cert.pem \
    WOLF_PULSE_IMAGE=ghcr.io/games-on-whales/pulseaudio:master \
    WOLF_RENDER_NODE=/dev/dri/renderD128 WOLF_DOCKER_SOCKET=/var/run/docker.sock \
    WOLF_DEFAULT_RUN_UID=1000 WOLF_DEFAULT_RUN_GID=1000 WOLF_STOP_CONTAINER_ON_EXIT=TRUE \
    HOST_APPS_STATE_FOLDER=/etc/wolf XDG_RUNTIME_DIR=/tmp/sockets \
    RUST_LOG=WARN RUST_BACKTRACE=full GST_DEBUG=2 PUID=0 PGID=0 UNAME=root
EXPOSE 47984/tcp 47989/tcp 47999/udp 48010/tcp 48100/udp 48200/udp
ENTRYPOINT ["/entrypoint.sh"]
