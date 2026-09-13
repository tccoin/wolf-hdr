# Rebuild gst-wayland-display from this checkout on the known HDR-capable
# GStreamer base. The runtime assembly copies the resulting plugin together
# with the freshly-built Wolf binary.
ARG GST_HDR_BASE_IMAGE=local/gst-wayland-display:vulkan-hdr
FROM ${GST_HDR_BASE_IMAGE}

ARG GSTREAMER_VERSION=1.28.4

# nvh265enc's GL interop is compiled only when the matching GStreamer GL ABI is
# present. Build the minimal EGL/surfaceless GL stack once here so the Rust
# compositor plugin can also link its AB30 -> GLMemory bridge against it.
RUN git clone --depth 1 --branch "${GSTREAMER_VERSION}" \
      https://gitlab.freedesktop.org/gstreamer/gstreamer.git /tmp/gstreamer && \
    find /tmp/gstreamer -path '*/docs/meson.build' -type f -print0 | \
      xargs -0 -r -I{} sh -c 'printf "%s\\n" "if not get_option('"'"'doc'"'"').allowed()" "  subdir_done()" "endif" | cat - "$1" >"$1.tmp" && mv "$1.tmp" "$1"' sh {} && \
    meson setup /tmp/gl-build /tmp/gstreamer \
      --prefix=/opt/gst --libdir=lib64 \
      -Dauto_features=disabled \
      -Dbase=enabled -Dtools=enabled \
      -Dgst-plugins-base:app=enabled \
      -Dgst-plugins-base:gl=enabled \
      -Dgst-plugins-bad:gl=enabled \
      -Dgst-plugins-base:gl_winsys=egl,gbm,surfaceless \
      -Dgst-plugins-base:videotestsrc=enabled \
      -Dgst-plugins-base:videoconvertscale=enabled \
      -Ddoc=disabled -Dtests=disabled -Dexamples=disabled -Dintrospection=disabled && \
    meson compile -C /tmp/gl-build && \
    meson install -C /tmp/gl-build

COPY third_party/gst-wayland-display/ /src/
WORKDIR /src

RUN cargo cinstall --release \
      --features cuda,gl-hdr \
      --prefix=/opt/gst \
      --libdir=/opt/gst/lib64/gstreamer-1.0 \
      --pkgconfigdir=/opt/gst/lib64/pkgconfig
