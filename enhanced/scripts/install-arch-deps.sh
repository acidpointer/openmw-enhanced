#!/usr/bin/env bash
set -euo pipefail

# Bootstrap disposable Arch build containers, never the user's host implicitly.
mode="${1:-dev}"
case "$mode" in dev|appimage) ;; *) echo "Usage: $0 dev|appimage" >&2; exit 2 ;; esac
[[ -f /etc/arch-release && "$EUID" == 0 && "${IN_OPENMW_ENHANCED_CONTAINER:-0}" == 1 ]] || {
    echo 'Run this bootstrap as root inside the Arch build container.' >&2
    exit 1
}
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -f "${script_dir}/archlinux/PKGBUILD" ]]; then
    recipe="${script_dir}/archlinux/PKGBUILD"
else
    recipe="${script_dir}/../../archlinux/PKGBUILD"
fi

source "$recipe"

# Arch does not support partial upgrades. Keep keyring/system/dependencies in
# one transaction instead of installing against stale repository databases.
pacman -Syu --noconfirm --needed archlinux-keyring base-devel \
    "${depends[@]}" "${makedepends[@]}" \
    ccache curl file pkgconf python qt6-wayland

if [[ "$mode" == appimage ]]; then
    pacman -S --noconfirm --needed desktop-file-utils fuse2 patchelf wget
    install -d /opt/linuxdeploy
    for tool in linuxdeploy linuxdeploy-plugin-qt; do
        curl --fail --silent --show-error --location --retry 3 \
            "https://github.com/linuxdeploy/${tool}/releases/download/continuous/${tool}-x86_64.AppImage" \
            --output "/opt/linuxdeploy/${tool}-x86_64.AppImage"
        chmod +x "/opt/linuxdeploy/${tool}-x86_64.AppImage"
    done
fi

# These are disposable container layers. Retain installed packages and their
# database, but do not bake another copy of every downloaded archive into them.
pacman -Scc --noconfirm
