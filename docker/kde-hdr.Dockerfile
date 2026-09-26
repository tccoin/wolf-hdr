# Build from the repository root; all inputs are public sources or this tree.
ARG KDE_BASE=ghcr.io/selkies-project/selkies-egl-desktop:26.04@sha256:4389c08124ae18f994c8de341e980a5aea95eb700ae216108c09ade026576111
FROM ${KDE_BASE} AS kwin-build
USER root
ARG BUILD_JOBS=16
ARG KWIN_VERSION=4:6.6.6-0ubuntu0.1
WORKDIR /work
RUN sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/ubuntu.sources \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get build-dep -y kwin=${KWIN_VERSION} \
 && apt-get source kwin=${KWIN_VERSION}
COPY docker/patches/kwin-wayland-*.patch /tmp/kwin-patches/
WORKDIR /work/kwin-6.6.6
RUN set -eu; for name in hdr-capabilities native-pq hdr-calibration wolf-wheel explicit-game-colors; do \
      patch -p1 < /tmp/kwin-patches/kwin-wayland-${name}.patch || exit 1; \
    done \
 && DEB_BUILD_OPTIONS=nocheck dpkg-buildpackage -b -uc -us -j${BUILD_JOBS}

FROM ${KDE_BASE} AS gamescope-build
USER root
ARG BUILD_JOBS=16
ARG GAMESCOPE_REF=ad2763da1c48860f649abfe842a087188dcb6e20
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake git libavif-dev libcap-dev libdecor-0-dev \
      libdisplay-info-dev libeis-dev libinput-dev libliftoff-dev libpipewire-0.3-dev \
      libseat-dev libvulkan-dev libwayland-dev libwlroots-0.19-dev libx11-dev libx11-xcb-dev \
      libxcb-composite0-dev libxcb-ewmh-dev libxcb-icccm4-dev libxcb-res0-dev \
      libxcb-xfixes0-dev libxcomposite-dev libxdamage-dev libxkbcommon-dev libxmu-dev \
      libxrender-dev libxcursor-dev libxxf86vm-dev libxres-dev libxtst-dev \
      libluajit-5.1-dev glslang-tools meson ninja-build pkg-config wayland-protocols \
 && git clone --filter=blob:none --no-checkout https://github.com/ValveSoftware/gamescope.git /work/gamescope \
 && git -C /work/gamescope fetch --depth=1 origin ${GAMESCOPE_REF} \
 && git -C /work/gamescope checkout --detach ${GAMESCOPE_REF} \
 && git -C /work/gamescope submodule update --init --recursive --depth=1
COPY docker/patches/gamescope-*.patch /tmp/gamescope-patches/
WORKDIR /work/gamescope
RUN set -eu; for name in wsi-overlay-bootstrap wsi-auto-hdr10 overlay-focus-restore ubuntu-2604-hdmi-header wayland-pointer-not-touch wayland-restore-active-pointer-constraint wayland-pq-reference shader-worker-shutdown nested-hdr-edid; do \
      git apply /tmp/gamescope-patches/gamescope-${name}.patch || exit 1; \
    done \
 && meson setup /work/gamescope-build . \
      -Denable_gamescope=true -Denable_gamescope_wsi_layer=true \
      -Denable_openvr_support=false -Denable_tests=false -Denable_zenity=false \
      -Dsdl2_backend=disabled -Dpipewire=disabled -Dinput_emulation=disabled \
 && ninja -C /work/gamescope-build -j${BUILD_JOBS}

# Optional regression-test artifact, built against the same protocol versions.
# Export only the small client with --target hdr-probe --output type=local,dest=...
FROM gamescope-build AS hdr-probe-build
WORKDIR /work/hdr-probe
COPY tests/platforms/linux/hdr-pq-patches.c ./
RUN set -eu; \
    for name in xdg-shell color-management gamescope-swapchain; do \
      case "$name" in \
        xdg-shell) xml=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml ;; \
        color-management) xml=/usr/share/wayland-protocols/staging/color-management/color-management-v1.xml ;; \
        gamescope-swapchain) xml=/work/gamescope/protocol/gamescope-swapchain.xml ;; \
      esac; \
      wayland-scanner client-header "$xml" "$name-client.h"; \
      wayland-scanner private-code "$xml" "$name-protocol.c"; \
    done; \
    cc -O2 -o hdr-pq-patches hdr-pq-patches.c *-protocol.c -lwayland-client -lX11 -lm

FROM scratch AS hdr-probe
COPY --from=hdr-probe-build /work/hdr-probe/hdr-pq-patches /hdr-pq-patches

# Optional Windows-side check for Wine's cached Advanced Color capability.
# The cross compiler is not installed in the desktop runtime image.
FROM ${KDE_BASE} AS wine-hdr-probe-build
USER root
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends gcc-mingw-w64-x86-64-posix
COPY tests/platforms/linux/wine-hdr-capability.c /work/wine-hdr-capability.c
RUN x86_64-w64-mingw32-gcc -O2 -Wall -Wextra -o /work/wine-hdr-capability.exe /work/wine-hdr-capability.c -luser32 -ldxgi -ldxguid

FROM scratch AS wine-hdr-probe
COPY --from=wine-hdr-probe-build /work/wine-hdr-capability.exe /wine-hdr-capability.exe

# Build the small DualSense adaptive-trigger test utility against the same
# SDL2 GameController API available in the desktop runtime.
FROM ${KDE_BASE} AS dualsense-trigger-test-build
USER root
RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends build-essential libsdl2-dev \
 && rm -rf /var/lib/apt/lists/*
COPY tools/dualsense-trigger-test.c /work/dualsense-trigger-test.c
RUN cc -O2 -Wall -Wextra -Werror -o /work/wolf-dualsense-trigger-test /work/dualsense-trigger-test.c \
      $(pkg-config --cflags --libs sdl2)

FROM ${KDE_BASE} AS runner
ENTRYPOINT []
HEALTHCHECK NONE
USER root
COPY --from=kwin-build /work/kwin-wayland_*.deb /tmp/kwin-debs/
COPY --from=kwin-build /work/libkwin6_*.deb /tmp/kwin-debs/
RUN dpkg -i /tmp/kwin-debs/*.deb \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends kscreen libluajit-5.1-2 \
 && rm -rf /tmp/kwin-debs /var/lib/apt/lists/* \
 && chown root:root /usr/bin/bwrap && chmod 4755 /usr/bin/bwrap \
 && for arch in x86_64-linux-gnu i386-linux-gnu; do \
      test -f /usr/lib/$arch/libudev.so.1.7.12; \
      ln -sf libudev.so.1.7.12 /usr/lib/$arch/libudev.so.1; \
      ln -sf libudev.so.1.7.12 /usr/lib/$arch/libudev.so; \
    done
COPY --from=gamescope-build /work/gamescope-build/src/gamescope /usr/games/gamescope
COPY --from=gamescope-build /work/gamescope-build/src/gamescopereaper /usr/local/bin/gamescopereaper
COPY --from=gamescope-build /work/gamescope-build/layer/libVkLayer_FROG_gamescope_wsi_x86_64.so /usr/local/lib/x86_64-linux-gnu/
COPY --from=gamescope-build /work/gamescope-build/layer/VkLayer_FROG_gamescope_wsi.x86_64.json /usr/share/vulkan/implicit_layer.d/
COPY docker/restore-chrome-sandbox.py /usr/local/share/wolf/restore-chrome-sandbox.py
RUN python3 /usr/local/share/wolf/restore-chrome-sandbox.py \
 && chown -R root:root /opt/google/chrome \
 && chmod 4755 /opt/google/chrome/chrome-sandbox
# Match the existing Heroic runner: sharing profiles with a newer launcher
# could migrate its settings behind the old runner's back. Pin the public deb.
ARG HEROIC_VERSION=2.21.0
ARG HEROIC_SHA256=dd862805f2e5b13efdf1c689d003bc22a5c5f9149a893a187fe18020d67e8e66
RUN curl -fL --retry 3 -o /tmp/heroic.deb \
      https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher/releases/download/v${HEROIC_VERSION}/Heroic-${HEROIC_VERSION}-linux-amd64.deb \
 && echo "${HEROIC_SHA256}  /tmp/heroic.deb" | sha256sum -c - \
 && apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --allow-downgrades /tmp/heroic.deb \
 && rm /tmp/heroic.deb \
 && rm -rf /var/lib/apt/lists/* \
 && chown -R root:root /opt/Heroic \
 && chmod 4755 /opt/Heroic/chrome-sandbox
COPY --chmod=0755 docker/wolf-kde-heroic.sh /usr/local/bin/heroic
COPY --chmod=0755 docker/wolf-heroic-game.sh /usr/local/bin/wolf-heroic-game
COPY docker/heroic-profile-init.py /usr/local/share/wolf/heroic-profile-init.py
COPY docker/heroic-game-defaults.py /usr/local/share/wolf/heroic-game-defaults.py
COPY docker/kde/heroic.desktop /usr/share/applications/heroic.desktop
COPY --from=dualsense-trigger-test-build /work/wolf-dualsense-trigger-test /usr/local/bin/wolf-dualsense-trigger-test
COPY docker/kde/dualsense-trigger-test.desktop /usr/share/applications/dualsense-trigger-test.desktop
COPY docker/kde/dualsense-trigger-test.desktop /usr/local/share/wolf/dualsense-trigger-test.desktop
COPY docker/kde/dlssnr.desktop /usr/local/share/wolf/dlssnr.desktop
COPY --chmod=0755 docker/wolf-dlssnr-gui.sh /usr/local/bin/wolf-dlssnr-gui
COPY --chmod=0755 docker/wolf-return-to-ui.sh /usr/local/bin/wolf-return-to-ui
COPY docker/kde/return-to-wolf-ui.desktop /usr/local/share/wolf/return-to-wolf-ui.desktop
COPY --chmod=0755 docker/wolf-selkies-kwin-entrypoint.sh /usr/local/bin/wolf-selkies-kwin-entrypoint
COPY --chmod=0755 docker/wolf-kde-steam.sh /usr/bin/steam
COPY --chmod=0755 docker/wolf-steam-game.sh /usr/local/bin/wolf-steam-game
COPY --chmod=0755 docker/steam-profile-init.sh /usr/local/bin/wolf-steam-profile-init
COPY docker/steam-game-defaults.py /usr/local/share/wolf/steam-game-defaults.py
COPY --chmod=0755 docker/steamos-session-select /usr/bin/steamos-session-select
COPY docker/kde-raise-steam.js docker/steam-running-games.py docker/kde/steam.desktop docker/kde/steam-big-picture.desktop /usr/local/share/wolf/
COPY docker/kde/heroic.desktop /usr/local/share/wolf/
USER ubuntu
CMD ["/usr/local/bin/wolf-selkies-kwin-entrypoint"]
