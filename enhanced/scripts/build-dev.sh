#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/build-dev.sh

Build OpenMW inside Docker without packaging an AppImage.
The CMake build tree is stored in .build/archlinux/cmake/dev.

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

IMAGE="${OPENMW_ENHANCED_DEV_IMAGE:-openmw-enhanced-dev:archlinux}"

if [[ "${IN_OPENMW_ENHANCED_CONTAINER:-0}" != "1" ]]; then
    build_toolchain_image "$ROOT" dev "$IMAGE"

    mount_args=()
    while IFS= read -r -d '' item; do
        mount_args+=("${item}")
    done < <(docker_mount_args "${ROOT}")

    docker_tty_args=()
    if [[ -t 1 ]]; then docker_tty_args+=(-t); fi
    exec docker run --rm "${docker_tty_args[@]}" \
        -e IN_OPENMW_ENHANCED_CONTAINER=1 \
        -e OPENMW_SOURCE_DIR="${OPENMW_SOURCE_DIR:-}" \
        -e CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
        -e OPENMW_LTO_BUILD="${OPENMW_LTO_BUILD:-ON}" \
        -e OPENMW_USE_MIMALLOC="${OPENMW_USE_MIMALLOC:-ON}" \
        -e OPENMW_MIMALLOC_DIAGNOSTICS="${OPENMW_MIMALLOC_DIAGNOSTICS:-OFF}" \
        -e OPENMW_BUILD_JOBS="${OPENMW_BUILD_JOBS:-}" \
        -e CCACHE_DIR=/ccache \
        "${mount_args[@]}" \
        -w /src \
        "${IMAGE}" \
        /src/enhanced/scripts/build-dev.sh "$@"
fi

require_arch_container
SOURCE_DIR="$(find_openmw_source_dir "$ROOT")"
git config --global --add safe.directory "$SOURCE_DIR"
BUILD_DIR="${OPENMW_BUILD_ROOT:-/build}/dev"
JOBS="$(cmake_jobs)"

mkdir -p "${CCACHE_DIR:-/ccache}"

openmw_configure "${SOURCE_DIR}" "${BUILD_DIR}"

cmake --build "${BUILD_DIR}" --parallel "${JOBS}"
