#!/usr/bin/env bash
set -euo pipefail

source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

ROOT="$(enhanced_root)"
ensure_cache_dirs "${ROOT}"

IMAGE="${OPENMW_ENHANCED_APPIMAGE_IMAGE:-openmw-enhanced-appimage:ubuntu-24.04}"

docker build \
    -f "${ROOT}/enhanced/docker/Dockerfile.appimage" \
    -t "${IMAGE}" \
    "${ROOT}"

mount_args=()
while IFS= read -r -d '' item; do
    mount_args+=("${item}")
done < <(docker_mount_args "${ROOT}")

exec docker run --rm -it \
    -e IN_OPENMW_ENHANCED_CONTAINER=1 \
    -e CCACHE_DIR=/ccache \
    -e CARGO_HOME=/cargo \
    "${mount_args[@]}" \
    -w /src \
    "${IMAGE}" \
    bash
