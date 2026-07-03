#!/usr/bin/env bash
set -euo pipefail

APPDIR="${1:-/build/appimage/AppDir}"
DIST_DIR="${DIST_DIR:-/dist}"
ROOT="/src"
BUILD_DIR="${BUILD_DIR:-$(dirname -- "${APPDIR}")/build}"
LINUXDEPLOY="${LINUXDEPLOY:-/opt/linuxdeploy/linuxdeploy-x86_64.AppImage}"

if [[ ! -x "${APPDIR}/usr/bin/openmw-launcher" ]]; then
    echo "Missing ${APPDIR}/usr/bin/openmw-launcher; build/install did not complete." >&2
    exit 1
fi

mkdir -p \
    "${APPDIR}/usr/share/applications" \
    "${APPDIR}/usr/share/icons/hicolor/256x256/apps" \
    "${APPDIR}/usr/lib/osgPlugins" \
    "${DIST_DIR}"

cp "${ROOT}/enhanced/appimage/openmw-enhanced.desktop" \
    "${APPDIR}/usr/share/applications/openmw-enhanced.desktop"

if [[ -f "${APPDIR}/usr/share/pixmaps/openmw.png" ]]; then
    cp "${APPDIR}/usr/share/pixmaps/openmw.png" \
        "${APPDIR}/usr/share/icons/hicolor/256x256/apps/openmw-enhanced.png"
fi

if [[ -f "${ROOT}/enhanced/appimage/AppRun" ]]; then
    cp "${ROOT}/enhanced/appimage/AppRun" "${APPDIR}/AppRun"
    chmod +x "${APPDIR}/AppRun"
fi

if [[ -f "${BUILD_DIR}/openmw.cfg" ]]; then
    cp "${BUILD_DIR}/openmw.cfg" "${APPDIR}/usr/bin/openmw.cfg"
    sed -i \
        -e 's|^resources=.*|resources=../share/games/openmw/resources|' \
        -e 's|^data=./resources/vfs-mw$|data=../share/games/openmw/resources/vfs-mw|' \
        "${APPDIR}/usr/bin/openmw.cfg"
fi

if [[ -f "${APPDIR}/etc/openmw/openmw-enhanced.cfg" ]]; then
    cp "${APPDIR}/etc/openmw/openmw-enhanced.cfg" "${APPDIR}/usr/bin/openmw-enhanced.cfg"
fi

for settings_file in defaults.bin defaults-cs.bin; do
    if [[ -f "${APPDIR}/etc/openmw/${settings_file}" ]]; then
        cp "${APPDIR}/etc/openmw/${settings_file}" "${APPDIR}/usr/bin/${settings_file}"
    fi
done

OSG_PLUGIN_DIR="$(find /usr/lib -type d -name 'osgPlugins-*' 2>/dev/null | head -n 1 || true)"
if [[ -n "${OSG_PLUGIN_DIR}" ]]; then
    for plugin in bmp dae dds freetype jpeg osg png serializers_osg tga; do
        find "${OSG_PLUGIN_DIR}" -maxdepth 1 -type f -name "osgdb_${plugin}.so" \
            -exec cp {} "${APPDIR}/usr/lib/osgPlugins/" \;
    done
fi

export APPIMAGE_EXTRACT_AND_RUN=1
export OUTPUT="${DIST_DIR}/OpenMW-Enhanced-x86_64.AppImage"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --desktop-file "${APPDIR}/usr/share/applications/openmw-enhanced.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/openmw-enhanced.png" \
    --executable "${APPDIR}/usr/bin/openmw-launcher" \
    --executable "${APPDIR}/usr/bin/openmw" \
    --plugin qt \
    --output appimage

echo "AppImage written to ${OUTPUT}"
