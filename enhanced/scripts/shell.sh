#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

ROOT="$(enhanced_root)"
ensure_cache_dirs "${ROOT}"

IMAGE="${OPENMW_ENHANCED_APPIMAGE_IMAGE:-openmw-enhanced-appimage:archlinux}"

build_toolchain_image "$ROOT" appimage "$IMAGE"

mount_args=()
while IFS= read -r -d '' item; do
    mount_args+=("${item}")
done < <(docker_mount_args "${ROOT}")

exec docker run --rm -it \
    -e IN_OPENMW_ENHANCED_CONTAINER=1 \
    -e CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
    -e OPENMW_LTO_BUILD="${OPENMW_LTO_BUILD:-ON}" \
    -e OPENMW_USE_MIMALLOC="${OPENMW_USE_MIMALLOC:-ON}" \
    -e OPENMW_MIMALLOC_DIAGNOSTICS="${OPENMW_MIMALLOC_DIAGNOSTICS:-OFF}" \
    -e OPENMW_BUILD_JOBS="${OPENMW_BUILD_JOBS:-}" \
    -e CCACHE_DIR=/ccache \
    "${mount_args[@]}" \
    -w /src \
    "${IMAGE}" \
    bash
