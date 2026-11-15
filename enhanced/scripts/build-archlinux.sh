#!/usr/bin/env bash
set -euo pipefail
source "$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)/common.sh"

install_args=(--install)
for arg in "$@"; do
    case "$arg" in
        --no-install) install_args=() ;;
        -h|--help)
            echo 'Usage: ./enhanced/scripts/build-archlinux.sh [--no-install]'
            echo 'Build and install the current working tree on Arch (as your normal user).'
            echo 'Packages are retained in .build/dist; makepkg may request sudo for pacman.'
            echo '--no-install builds the package without installing it (used by CI).'
            exit 0 ;;
        *) echo "Unknown argument: $arg. See --help." >&2; exit 2 ;;
    esac
done
require_arch_container
[[ "$EUID" != 0 ]] || { echo 'makepkg must run as a normal user, not root.' >&2; exit 1; }
ROOT="$(enhanced_root)"
ensure_cache_dirs "$ROOT"
work="${ROOT}/.build/archlinux/makepkg"
mkdir -p "$work"
# makepkg updates pkgver in its recipe. Do that in a disposable copy, never in
# the tracked PKGBUILD. --noextract builds the live checkout, including edits,
# instead of silently cloning an older remote commit.
cp -- "${ROOT}/archlinux/PKGBUILD" "${work}/PKGBUILD"
export OPENMW_SOURCE_DIR="$(find_openmw_source_dir "$ROOT")"
export BUILDDIR="$work"
export PKGDEST="${ROOT}/.build/dist"
export SRCDEST="${ROOT}/.build/archlinux/downloads"
export CCACHE_DIR="${CCACHE_DIR:-${ROOT}/.build/archlinux/ccache}"
export CMAKE_POLICY_VERSION_MINIMUM=3.5
cd -- "$work"
exec makepkg --syncdeps --noextract --force "${install_args[@]}"
