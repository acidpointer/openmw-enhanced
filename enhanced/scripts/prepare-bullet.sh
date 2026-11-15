#!/usr/bin/env bash
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"
require_arch_container

# Arch bullet-dp already enables multithreading. Build only the explicit OFF
# comparison variant, privately and statically, without replacing system Bullet.
version=3.25
checksum=7086e5fcf69635801bb311261173cb8d173b712ca1bd78be03df48fad884674e85512861190e45a1a62d5627aaad65cde08c175c44a3be9afa410d3dfd5358d4
downloads="${OPENMW_DOWNLOADS_DIR:-/downloads}"
work="${OPENMW_BUILD_ROOT:-/build}/bullet-unthreaded"
archive="${downloads}/bullet-${version}.tar.gz"
mkdir -p "$downloads" "$work"
if [[ ! -f "$archive" ]]; then
    curl --fail --silent --show-error --location --retry 3 \
        "https://github.com/bulletphysics/bullet3/archive/refs/tags/${version}.tar.gz" \
        --output "${archive}.pending"
    printf '%s  %s\n' "$checksum" "${archive}.pending" | sha512sum --check >&2
    mv -- "${archive}.pending" "$archive"
fi
printf '%s  %s\n' "$checksum" "$archive" | sha512sum --check >&2
if [[ ! -f "${work}/source/CMakeLists.txt" ]]; then
    mkdir -p "${work}/source"
    tar -xzf "$archive" --strip-components=1 -C "${work}/source"
fi
ensure_cmake_build_dir "${work}/source" "${work}/build"
cmake -S "${work}/source" -B "${work}/build" -G Ninja \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="${work}/install" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF -DINSTALL_LIBS=ON \
    -DBUILD_BULLET2_DEMOS=OFF -DBUILD_BULLET3=OFF -DBUILD_CPU_DEMOS=OFF \
    -DBUILD_EXTRAS=OFF -DBUILD_OPENGL3_DEMOS=OFF -DBUILD_UNIT_TESTS=OFF \
    -DUSE_DOUBLE_PRECISION=ON -DBULLET2_MULTITHREADING=OFF -DUSE_GLUT=OFF >&2
cmake --build "${work}/build" --parallel "$(cmake_jobs)" >&2
cmake --install "${work}/build" >&2
printf '%s\n' "${work}/install"
