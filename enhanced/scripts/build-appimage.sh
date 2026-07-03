#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/build-appimage.sh [--bullet-threading on|off]

Build OpenMW inside Docker and package it as:
  .build/dist/OpenMW-Enhanced-x86_64.AppImage

Options:
  --bullet-threading on|off  Select the bundled Bullet build. Default: on.
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
IMAGE="${OPENMW_ENHANCED_APPIMAGE_IMAGE:-openmw-enhanced-appimage:ubuntu-24.04}"

if [[ "${IN_OPENMW_ENHANCED_CONTAINER:-0}" != "1" ]]; then
    docker build \
        -f "${ROOT}/enhanced/docker/Dockerfile.appimage" \
        -t "${IMAGE}" \
        "${ROOT}"

    mount_args=()
    while IFS= read -r -d '' item; do
        mount_args+=("${item}")
    done < <(docker_mount_args "${ROOT}")

    exec docker run --rm -t \
        -e IN_OPENMW_ENHANCED_CONTAINER=1 \
        -e OPENMW_SOURCE_DIR="${OPENMW_SOURCE_DIR:-}" \
        -e OPENMW_BULLET_THREADING="${BULLET_THREADING}" \
        -e CCACHE_DIR=/ccache \
        -e CARGO_HOME=/cargo \
        "${mount_args[@]}" \
        -w /src \
        "${IMAGE}" \
        /src/enhanced/scripts/build-appimage.sh "$@"
fi

SOURCE_DIR="$(find_openmw_source_dir /src)"
BUILD_ROOT="/build/appimage"
BUILD_DIR="${BUILD_ROOT}/build"
APPDIR="${BUILD_ROOT}/AppDir"
JOBS="$(cmake_jobs)"

case "${OPENMW_BULLET_THREADING:-on}" in
    on) BULLET_PREFIX="/opt/openmw-enhanced/bullet-threaded" ;;
    off) BULLET_PREFIX="/opt/openmw-enhanced/bullet-unthreaded" ;;
    *)
        echo "Invalid OPENMW_BULLET_THREADING=${OPENMW_BULLET_THREADING}. Use on or off." >&2
        exit 2
        ;;
esac

rm -rf "${APPDIR}"
ensure_cmake_build_dir "${SOURCE_DIR}" "${BUILD_DIR}"
mkdir -p "${APPDIR}" /ccache /cargo
rm -rf "${BUILD_DIR}/resources/vfs"

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_PREFIX_PATH="${BULLET_PREFIX}" \
    -DBULLET_ROOT="${BULLET_PREFIX}" \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DOPENMW_USE_SYSTEM_BULLET=ON \
    -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON \
    -DDEPLOY_QT_TRANSLATIONS=ON

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
DESTDIR="${APPDIR}" cmake --install "${BUILD_DIR}"

/src/enhanced/scripts/package-appimage.sh "${APPDIR}"
