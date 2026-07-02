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
        "${root}/.build/docker" \
        "${root}/.build/cmake" \
        "${root}/.build/ccache" \
        "${root}/.build/cargo" \
        "${root}/.build/appdir" \
        "${root}/.build/dist"
}

docker_mount_args() {
    local root="$1"
    local volume_suffix="${OPENMW_DOCKER_VOLUME_SUFFIX:-:z}"
    printf '%s\0' \
        -v "${root}:/src${volume_suffix}" \
        -v "${root}/.build/cmake:/build${volume_suffix}" \
        -v "${root}/.build/ccache:/ccache${volume_suffix}" \
        -v "${root}/.build/cargo:/cargo${volume_suffix}" \
        -v "${root}/.build/appdir:/appdir-cache${volume_suffix}" \
        -v "${root}/.build/dist:/dist${volume_suffix}"
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
    if command -v nproc >/dev/null 2>&1; then
        nproc
    else
        printf '2\n'
    fi
}

ensure_cmake_build_dir() {
    local source_dir="$1"
    local build_dir="$2"
    local cache_file="${build_dir}/CMakeCache.txt"

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
}
