#!/usr/bin/env bash
set -euo pipefail
[[ $# == 2 ]] || { echo "Usage: $0 CMAKE_BUILD_DIR INSTALL_ROOT" >&2; exit 2; }
cache="${1}/CMakeCache.txt"
root="$2"
value="$(sed -n 's/^OPENMW_USE_MIMALLOC:BOOL=//p' "$cache")"
case "${value^^}" in
    OFF|FALSE|0|NO) exit 0 ;;
    ON|TRUE|1|YES) ;;
    *) echo "Unknown OPENMW_USE_MIMALLOC value: $value" >&2; exit 1 ;;
esac
shopt -s nullglob
checked=0
for binary in "${root}/usr/bin/"*; do
    [[ -f "$binary" && -x "$binary" ]] || continue
    if ! dynamic="$(readelf --dyn-syms --wide "$binary" 2>/dev/null)"; then continue; fi
    if [[ "$binary" == *.so ]]; then
        if awk '$7 != "UND" && $8 ~ /^mi_/ { found=1 } END { exit !found }' <<< "$dynamic"; then
            echo "A renderer/module embeds its own allocator: $binary" >&2
            exit 1
        fi
        continue
    fi
    checked=$((checked + 1))
    for symbol in malloc calloc realloc free aligned_alloc posix_memalign mi_version; do
        if ! awk -v name="$symbol" '$7 != "UND" && $8 == name { found=1 } END { exit !found }' <<< "$dynamic"; then
            echo "Missing static allocator override $symbol in $binary" >&2
            exit 1
        fi
    done
    for symbol in '^_Znw[mjy]$' '^_ZdlPv$'; do
        if ! awk -v name="$symbol" '$7 != "UND" && $8 ~ name { found=1 } END { exit !found }' <<< "$dynamic"; then
            echo "Missing static C++ allocator override $symbol in $binary" >&2
            exit 1
        fi
    done
    if readelf -d "$binary" | awk '/NEEDED.*lib(mimalloc|openmw-mimalloc)/ { found=1 } END { exit !found }'; then
        echo "Unexpected shared mimalloc dependency in $binary" >&2
        exit 1
    fi
done
if (( checked == 0 )); then
    echo "No installed ELF executables found for static allocator validation in $root" >&2
    exit 1
fi
