# Build only the Gamescope WSI layer from a pinned upstream revision.  The
# Steam image ships an older layer that fails to recognise Pressure Vessel's
# gamescope socket layout, so Proton silently falls back to a non-HDR
# swapchain even though Gamescope is running with --hdr-enabled.
FROM ghcr.io/games-on-whales/steam:edge AS gamescope_wsi_builder

ARG GAMESCOPE_REF=05949f8149bb5d16b006624d319a76e2433caf4c

COPY docker/patches/gamescope-wsi-overlay-bootstrap.patch /tmp/gamescope-wsi-overlay-bootstrap.patch

RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        ca-certificates cmake g++ git libvulkan-dev libwayland-dev \
        libx11-dev libx11-xcb-dev libxcb1-dev libxcb-composite0-dev meson ninja-build pkg-config \
        wayland-protocols \
    && rm -rf /var/lib/apt/lists/* \
    && git clone --filter=blob:none --no-checkout https://github.com/ValveSoftware/gamescope.git /src/gamescope \
    && cd /src/gamescope \
    && git fetch --depth=1 origin "${GAMESCOPE_REF}" \
    && git checkout --detach "${GAMESCOPE_REF}" \
    && git submodule update --init --depth=1 subprojects/vkroots \
    && git apply /tmp/gamescope-wsi-overlay-bootstrap.patch \
    && meson setup /build/gamescope . \
        -Denable_gamescope=false \
        -Denable_gamescope_wsi_layer=true \
        -Denable_openvr_support=false \
        -Denable_tests=false \
        -Denable_zenity=false \
    && ninja -C /build/gamescope

FROM ghcr.io/games-on-whales/steam:edge

COPY --from=gamescope_wsi_builder /build/gamescope/layer/libVkLayer_FROG_gamescope_wsi_x86_64.so /usr/lib/x86_64-linux-gnu/gamescope/libVkLayer_FROG_gamescope_wsi_x86_64.so

# Wolf hot-plugs the virtual keyboard and mouse just after a runner is created.
# Starting Gamescope before those nodes exist leaves it without a libinput seat.
RUN mv /opt/gow/startup.sh /opt/gow/startup-default.sh
COPY docker/steam-wait-input.sh /opt/gow/startup.sh
COPY docker/steam-runtime-dirs-init.sh /etc/cont-init.d/00-steam-runtime-dirs.sh
COPY docker/steam-safe-user-init.sh /etc/cont-init.d/10-setup_user.sh
COPY docker/steam-uinput-init.sh /etc/cont-init.d/20-steam-uinput.sh
COPY docker/steam-wait-wayland.sh /opt/gow/steam-wait-wayland.sh
COPY docker/cyberpunk-hdr-startup.sh /opt/gow/cyberpunk-hdr-startup.sh
COPY docker/steam-hdr-startup.sh /opt/gow/steam-hdr-startup.sh
COPY docker/steam-adaptive-startup.sh /opt/gow/steam-adaptive-startup.sh
COPY docker/steam-hdr-game-env.sh /opt/gow/steam-hdr-game-env.sh
# Steam Input needs a live, read-only /dev/input bind so the XInput device it
# creates through uinput is visible to Proton.  The upstream helper attempts
# to chmod device nodes; on a read-only bind that is expected to fail.
RUN sed -i 's/chmod g+rw "\$dev"/chmod g+rw "\$dev" || true/' /opt/gow/ensure-groups \
    && chmod 0755 /opt/gow/startup.sh /etc/cont-init.d/00-steam-runtime-dirs.sh /etc/cont-init.d/10-setup_user.sh /etc/cont-init.d/20-steam-uinput.sh /opt/gow/steam-wait-wayland.sh /opt/gow/cyberpunk-hdr-startup.sh /opt/gow/steam-hdr-startup.sh /opt/gow/steam-adaptive-startup.sh /opt/gow/steam-hdr-game-env.sh
