#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/build-appimage.sh [--bullet-threading on|off]

Build OpenMW inside Docker and package it as:
  .build/dist/OpenMW-Enhanced-x86_64.AppImage

Options:
  --bullet-threading on|off  Arch bullet-dp, or a private unthreaded build. Default: on.
  -h, --help                 Show this help.
EOF
}

for arg in "$@"; do
    case "${arg}" in
        -h|--help)
            usage
            exit 0
            ;;
    esac
done

ROOT="$(enhanced_root)"
ensure_cache_dirs "${ROOT}"

BULLET_THREADING="$(parse_bullet_threading "$@")"
IMAGE="${OPENMW_ENHANCED_APPIMAGE_IMAGE:-openmw-enhanced-appimage:archlinux}"

if [[ "${IN_OPENMW_ENHANCED_CONTAINER:-0}" != "1" ]]; then
    build_toolchain_image "$ROOT" appimage "$IMAGE"

    mount_args=()
    while IFS= read -r -d '' item; do
        mount_args+=("${item}")
    done < <(docker_mount_args "${ROOT}")

    docker_tty_args=()
    if [[ -t 1 ]]; then
        docker_tty_args+=(-t)
    fi

    exec docker run --rm "${docker_tty_args[@]}" \
        -e IN_OPENMW_ENHANCED_CONTAINER=1 \
        -e OPENMW_SOURCE_DIR="${OPENMW_SOURCE_DIR:-}" \
        -e OPENMW_BULLET_THREADING="${BULLET_THREADING}" \
        -e CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
        -e OPENMW_LTO_BUILD="${OPENMW_LTO_BUILD:-ON}" \
        -e OPENMW_USE_MIMALLOC="${OPENMW_USE_MIMALLOC:-ON}" \
        -e OPENMW_MIMALLOC_DIAGNOSTICS="${OPENMW_MIMALLOC_DIAGNOSTICS:-OFF}" \
        -e OPENMW_BUILD_JOBS="${OPENMW_BUILD_JOBS:-}" \
        -e CCACHE_DIR=/ccache \
        "${mount_args[@]}" \
        -w /src \
        "${IMAGE}" \
        /src/enhanced/scripts/build-appimage.sh "$@"
fi

require_arch_container
SOURCE_DIR="$(find_openmw_source_dir "$ROOT")"
# The checkout is owned by the host runner/user; Git runs as root in Docker.
# Trust only this mounted source directory so release commit metadata is available.
git config --global --add safe.directory "${SOURCE_DIR}"
BUILD_ROOT="${OPENMW_BUILD_ROOT:-/build}/appimage/${BULLET_THREADING}"
BUILD_DIR="${BUILD_ROOT}/build"
APPDIR="${BUILD_ROOT}/AppDir"
JOBS="$(cmake_jobs)"

bullet_args=()
if [[ "$BULLET_THREADING" == off ]]; then
    BULLET_PREFIX="$("${ROOT}/enhanced/scripts/prepare-bullet.sh")"
    bullet_args=(-DCMAKE_PREFIX_PATH="$BULLET_PREFIX" -DBULLET_ROOT="$BULLET_PREFIX" -DBULLET_STATIC=ON)
fi

rm -rf "${APPDIR}"
mkdir -p "${APPDIR}" "${CCACHE_DIR:-/ccache}"
rm -rf "${BUILD_DIR}/resources/vfs"

openmw_configure "${SOURCE_DIR}" "${BUILD_DIR}" "${bullet_args[@]}"

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}"

"${ROOT}/enhanced/scripts/package-appimage.sh" "${APPDIR}"
