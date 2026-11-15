ARG ARCH_IMAGE=archlinux:base-devel
FROM ${ARCH_IMAGE} AS dev

# Keep the package recipe as the dependency authority for local builds and CI.
COPY archlinux/PKGBUILD /opt/openmw-enhanced/archlinux/PKGBUILD
COPY enhanced/scripts/install-arch-deps.sh /opt/openmw-enhanced/install-arch-deps.sh
RUN IN_OPENMW_ENHANCED_CONTAINER=1 /opt/openmw-enhanced/install-arch-deps.sh dev

ENV CCACHE_DIR=/ccache \
    CMAKE_POLICY_VERSION_MINIMUM=3.5

FROM dev AS appimage
RUN IN_OPENMW_ENHANCED_CONTAINER=1 /opt/openmw-enhanced/install-arch-deps.sh appimage
ENV PATH=/opt/linuxdeploy:${PATH} \
    QMAKE=/usr/bin/qmake6
