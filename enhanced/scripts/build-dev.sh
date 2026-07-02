#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/build-dev.sh

Build OpenMW inside Docker without packaging an AppImage.
The CMake build tree is stored in .build/cmake/dev.

Options:
  -h, --help  Show this help.
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
esac

ROOT="$(enhanced_root)"
ensure_cache_dirs "${ROOT}"

IMAGE="${OPENMW_ENHANCED_DEV_IMAGE:-openmw-enhanced-dev:ubuntu-24.04}"

if [[ "${IN_OPENMW_ENHANCED_CONTAINER:-0}" != "1" ]]; then
    docker build \
        -f "${ROOT}/enhanced/docker/Dockerfile.dev" \
        -t "${IMAGE}" \
        "${ROOT}"

    mount_args=()
    while IFS= read -r -d '' item; do
        mount_args+=("${item}")
    done < <(docker_mount_args "${ROOT}")

    exec docker run --rm -t \
        -e IN_OPENMW_ENHANCED_CONTAINER=1 \
        -e OPENMW_SOURCE_DIR="${OPENMW_SOURCE_DIR:-}" \
        -e CCACHE_DIR=/ccache \
        -e CARGO_HOME=/cargo \
        "${mount_args[@]}" \
        -w /src \
        "${IMAGE}" \
        /src/enhanced/scripts/build-dev.sh "$@"
fi

SOURCE_DIR="$(find_openmw_source_dir /src)"
BUILD_DIR="/build/dev"
JOBS="$(cmake_jobs)"

mkdir -p "${BUILD_DIR}" /ccache /cargo

cmake -S "${SOURCE_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}" \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON \
    -DDEPLOY_QT_TRANSLATIONS=ON

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
