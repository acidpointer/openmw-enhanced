#!/usr/bin/env bash
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

apply=0
files_only=0
for arg in "$@"; do
    case "$arg" in
        --apply) apply=1 ;;
        --files-only) files_only=1 ;;
        -h|--help)
            echo 'Usage: ./enhanced/scripts/cleanup-ubuntu-builds.sh [--apply] [--files-only]'
            echo 'Default: list obsolete project caches and exact Ubuntu toolchain images.'
            echo 'Retains .build/archlinux, .build/dist, user data and unrelated Docker resources.'
            exit 0 ;;
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done
ROOT="$(enhanced_root)"
[[ ! -L "${ROOT}/.build" ]] || { echo 'Refusing a symlinked .build directory.' >&2; exit 1; }
# These names belonged to the old Ubuntu toolchain exclusively. Arch has a
# separate namespace; never delete a current build tree or a finished artifact.
for name in cmake ccache cargo appdir docker; do
    path="${ROOT}/.build/${name}"
    if [[ -e "$path" || -L "$path" ]]; then
        printf 'Legacy cache: %s\n' "$path"
        if (( apply )); then rm -rf -- "$path"; fi
    fi
done
if (( !files_only )); then
    if ! command -v docker >/dev/null || ! docker info >/dev/null 2>&1; then
        echo 'Docker is unavailable; legacy Docker image cleanup remains pending.' >&2
        exit 1
    fi
    for image in openmw-enhanced-appimage:ubuntu-24.04 openmw-enhanced-dev:ubuntu-24.04; do
        if docker image inspect "$image" >/dev/null 2>&1; then
            printf 'Legacy image: %s\n' "$image"
            # Without --force, Docker refuses images still used by containers.
            if (( apply )); then docker image rm "$image"; fi
        fi
    done
fi
if (( !apply )); then echo 'Preview only. Use --apply to remove these exact items.'; fi
