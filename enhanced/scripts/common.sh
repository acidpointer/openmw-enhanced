#!/usr/bin/env bash
set -euo pipefail

enhanced_root() {
    local script_dir
    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
    cd -- "${script_dir}/../.." && pwd
}

find_openmw_source_dir() {
    local root="$1"

    if [[ -n "${OPENMW_SOURCE_DIR:-}" ]]; then
        cd -- "${OPENMW_SOURCE_DIR}" && pwd
        return
    fi

    if [[ -f "${root}/CMakeLists.txt" && -d "${root}/apps" && -d "${root}/components" ]]; then
        printf '%s\n' "${root}"
        return
    fi

    if [[ -f "${root}/src/openmw/CMakeLists.txt" ]]; then
        printf '%s\n' "${root}/src/openmw"
        return
    fi

    cat >&2 <<'EOF'
OpenMW source tree was not found.

Expected either:
  - OpenMW checked out at repository root, or
  - OpenMW checked out at src/openmw

Run:
  ./enhanced/scripts/fetch-upstream.sh

or set OPENMW_SOURCE_DIR=/path/to/openmw.
EOF
    exit 1
}

ensure_cache_dirs() {
    local root="$1"
    mkdir -p \
        "${root}/.build/archlinux/cmake" \
        "${root}/.build/archlinux/ccache" \
        "${root}/.build/archlinux/downloads" \
        "${root}/.build/dist"
}

docker_mount_args() {
    local root="$1"
    local volume_suffix="${OPENMW_DOCKER_VOLUME_SUFFIX:-:z}"
    printf '%s\0' \
        -v "${root}:/src${volume_suffix}" \
        -v "${root}/.build/archlinux/cmake:/build${volume_suffix}" \
        -v "${root}/.build/archlinux/ccache:/ccache${volume_suffix}" \
        -v "${root}/.build/archlinux/downloads:/downloads${volume_suffix}" \
        -v "${root}/.build/dist:/dist${volume_suffix}"
}

build_toolchain_image() {
    local root="$1" target="$2" image="$3"
    docker build --pull --target "$target" \
        --build-arg "ARCH_IMAGE=${OPENMW_ARCH_IMAGE:-archlinux:base-devel}" \
        -f "${root}/enhanced/docker/Dockerfile" -t "$image" "$root"
}

parse_bullet_threading() {
    local value="on"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -h|--help)
                return 0
                ;;
            --bullet-threading)
                if [[ $# -lt 2 ]]; then
                    echo "--bullet-threading requires on or off" >&2
                    exit 2
                fi
                value="$2"
                shift 2
                ;;
            --bullet-threading=*)
                value="${1#*=}"
                shift
                ;;
            *)
                echo "Unknown argument: $1" >&2
                exit 2
                ;;
        esac
    done

    case "${value}" in
        on|off) printf '%s\n' "${value}" ;;
        *)
            echo "Invalid --bullet-threading value: ${value}. Use on or off." >&2
            exit 2
            ;;
    esac
}

cmake_jobs() {
    if [[ -n "${OPENMW_BUILD_JOBS:-}" ]]; then
        [[ "$OPENMW_BUILD_JOBS" =~ ^[1-9][0-9]*$ ]] || {
            echo 'OPENMW_BUILD_JOBS must be a positive integer.' >&2
            return 2
        }
        printf '%s\n' "$OPENMW_BUILD_JOBS"
    elif command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

# Shared by the PKGBUILD, development builds and AppImage installation. Keep
# install/config paths identical so every format uses existing OpenMW settings.
openmw_configure() {
    local source_dir="$1" build_dir="$2"
    shift 2
    ensure_cmake_build_dir "$source_dir" "$build_dir"
    local -a launcher=()
    if command -v ccache >/dev/null 2>&1; then
        launcher=(-DCMAKE_CXX_COMPILER_LAUNCHER=ccache -DCMAKE_C_COMPILER_LAUNCHER=ccache)
    fi
    if command -v mold >/dev/null 2>&1; then
        launcher+=(-DCMAKE_LINKER_TYPE=MOLD)
    fi
    cmake -S "$source_dir" -B "$build_dir" -G Ninja \
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}" \
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_SYSCONFDIR=/etc \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_DATADIR=share/games \
        -DOPENMW_LOCAL_INSTALL=OFF \
        -DOPENMW_USE_MIMALLOC="${OPENMW_USE_MIMALLOC:-ON}" \
        -DOPENMW_MIMALLOC_DIAGNOSTICS="${OPENMW_MIMALLOC_DIAGNOSTICS:-OFF}" \
        -DOPENMW_LTO_BUILD="${OPENMW_LTO_BUILD:-ON}" \
        -DOPENMW_USE_SYSTEM_BULLET=ON \
        -DBULLET_STATIC=OFF \
        -DOSG_STATIC=OFF \
        -DOPENMW_USE_SYSTEM_RECASTNAVIGATION=ON \
        -DRECASTNAVIGATION_STATIC=OFF \
        -DOPENMW_BUILD_VULKAN_RENDERER=ON \
        -DDEPLOY_QT_TRANSLATIONS=ON \
        "${launcher[@]}" "$@"
}

require_arch_container() {
    if [[ ! -f /etc/arch-release ]]; then
        echo 'The build container must be Arch Linux. Rebuild the Arch toolchain image.' >&2
        exit 1
    fi
}

ensure_cmake_build_dir() {
    local source_dir="$1"
    local build_dir="$2"
    local cache_file="${build_dir}/CMakeCache.txt"
    local toolchain="" old_toolchain=""
    if [[ -f /etc/arch-release ]] && command -v pacman >/dev/null 2>&1; then
        toolchain="$(
            (
                source "$(enhanced_root)/archlinux/PKGBUILD"
                pacman -Q | awk -v names="${depends[*]} ${makedepends[*]} gcc binutils linux-api-headers libglvnd freetype2 sdl2-compat" '
                    BEGIN {
                        n = split(names, list, " ");
                        for (i = 1; i <= n; ++i) { sub(/[<>=].*$/, "", list[i]); wanted[list[i]] = 1 }
                    }
                    $1 in wanted { print }
                '
            ) | LC_ALL=C sort | sha256sum | cut -d ' ' -f 1
        )"
        if [[ -f "${build_dir}/.openmw-toolchain" ]]; then
            read -r old_toolchain < "${build_dir}/.openmw-toolchain" || old_toolchain=""
        fi
        if [[ -f "$cache_file" && "$toolchain" != "$old_toolchain" ]]; then
            echo "Arch toolchain packages changed; discarding incompatible CMake objects in ${build_dir}." >&2
            rm -rf -- "$build_dir"
        fi
    fi

    if [[ -f "${cache_file}" ]]; then
        local cached_source_dir
        cached_source_dir="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "${cache_file}" | tail -n 1)"

        if [[ -n "${cached_source_dir}" && "${cached_source_dir}" != "${source_dir}" ]]; then
            echo "CMake source directory changed for ${build_dir}; clearing stale build cache." >&2
            echo "  cached:  ${cached_source_dir}" >&2
            echo "  current: ${source_dir}" >&2
            rm -rf "${build_dir}"
        fi
    fi

    mkdir -p "${build_dir}"
    if [[ -n "$toolchain" ]]; then
        printf '%s\n' "$toolchain" > "${build_dir}/.openmw-toolchain"
    fi
}
