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

# Match the plugins to the OSG libraries used by this build. OSG first searches
# for osgPlugins-VERSION/osgdb_NAME.so and only then tries a bare filename. A
# flattened plugin directory lets host plugins win that first search, even with
# OSG_LIBRARY_PATH set, mixing incompatible OSG ABIs in the same process.
OSG_VERSION="$(sed -n 's/^OPENSCENEGRAPH_VERSION:INTERNAL=//p' "${BUILD_DIR}/CMakeCache.txt")"
OSG_DB_LIBRARY="$(sed -n 's/^OSGDB_LIBRARY_RELEASE:FILEPATH=//p' "${BUILD_DIR}/CMakeCache.txt")"
if [[ -z "${OSG_VERSION}" || ! -f "${OSG_DB_LIBRARY}" ]]; then
    echo "Cannot determine the build's shared OSG library/version from CMakeCache.txt." >&2
    exit 1
fi
OSG_LIBRARY_DIR="$(dirname -- "${OSG_DB_LIBRARY}")"
OSG_SONAME="$(readelf -d "${OSG_DB_LIBRARY}" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')"
if [[ ! "${OSG_SONAME}" =~ ^libosgDB\.so\.([0-9]+)$ ]]; then
    echo "Cannot determine the build's OSG ABI from ${OSG_DB_LIBRARY}." >&2
    exit 1
fi
OSG_ABI="${BASH_REMATCH[1]}"
OSG_PLUGIN_DIR="${OSG_LIBRARY_DIR}/osgPlugins-${OSG_VERSION}"
OSG_PLUGIN_DEST="${APPDIR}/usr/lib/osgPlugins-${OSG_VERSION}"
mkdir -p "${OSG_PLUGIN_DEST}"
ln -sfn "osgPlugins-${OSG_VERSION}" "${APPDIR}/usr/lib/openmw-osg-plugins"

# Serializers lazily dlopen unversioned node-kit names (libosg.so, libosgFX.so,
# etc.). SONAME-only deployment leaves those lookups free to load host libraries
# with a different ABI. Keep the aliases and their targets from this build.
shopt -s nullglob
osg_libraries=("${OSG_LIBRARY_DIR}"/libosg*.so."${OSG_ABI}")
if (( ${#osg_libraries[@]} == 0 )); then
    echo "No OSG libraries found for ABI ${OSG_ABI}." >&2
    exit 1
fi
osg_dependency_args=()
for library in "${osg_libraries[@]}"; do
    library_name="$(basename -- "${library}")"
    library_soname="$(readelf -d "${library}" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')"
    if [[ "${library_soname}" != "${library_name}" ]]; then
        echo "Unexpected OSG library SONAME in ${library}: ${library_soname}" >&2
        exit 1
    fi
    cp -L "${library}" "${APPDIR}/usr/lib/${library_name}"
    ln -sfn "${library_name}" "${APPDIR}/usr/lib/${library_name%.${OSG_ABI}}"
    osg_dependency_args+=(--deploy-deps-only "${APPDIR}/usr/lib/${library_name}")
done

osg_plugins=(bmp dae dds deprecated_osg freetype glsl jpeg osg png serializers_osg tga)
declare -A included_plugins=()
for plugin in "${osg_plugins[@]}"; do
    included_plugins["${plugin}"]=1
done
# Both current and legacy scene readers load wrappers on demand. Keep those
# wrappers with the matching core libraries instead of falling back to the host.
for plugin_file in "${OSG_PLUGIN_DIR}"/osgdb_serializers_*.so "${OSG_PLUGIN_DIR}"/osgdb_deprecated_*.so; do
    plugin="$(basename -- "${plugin_file}" .so)"
    plugin="${plugin#osgdb_}"
    if [[ ! -v "included_plugins[${plugin}]" ]]; then
        osg_plugins+=("${plugin}")
        included_plugins["${plugin}"]=1
    fi
done

check_osg_dependencies() {
    local binary="$1" dependency dynamic
    local -a dependencies
    dynamic="$(readelf -d "${binary}")" || return 1
    mapfile -t dependencies < <(sed -n 's/.*(NEEDED).*\[\(libosg[^]]*\)\].*/\1/p' <<< "${dynamic}")
    for dependency in "${dependencies[@]}"; do
        if [[ "${dependency}" != *.so."${OSG_ABI}" || ! -f "${APPDIR}/usr/lib/${dependency}" ]]; then
            echo "OSG ABI mismatch or missing bundled dependency in ${binary}: ${dependency}" >&2
            exit 1
        fi
    done
}

for plugin in "${osg_plugins[@]}"; do
    plugin_file="${OSG_PLUGIN_DIR}/osgdb_${plugin}.so"
    if [[ ! -f "${plugin_file}" ]]; then
        echo "Missing required OSG plugin: ${plugin_file}" >&2
        exit 1
    fi
    cp -L "${plugin_file}" "${OSG_PLUGIN_DEST}/"
    # --library flattened a second physical copy into usr/lib. Loading the
    # versioned and bare names can register the same OSG wrappers twice.
    # Remove only these known stale deployment artifacts from cached AppDirs.
    rm -f -- "${APPDIR}/usr/lib/osgdb_${plugin}.so"
done
for binary in "${OSG_PLUGIN_DEST}"/osgdb_*.so "${APPDIR}"/usr/lib/libosg*.so."${OSG_ABI}"; do
    check_osg_dependencies "${binary}"
done

export APPIMAGE_EXTRACT_AND_RUN=1
# Publish only after the staged layout passes its checks. Keep an existing
# working AppImage intact if deployment or validation fails.
export OUTPUT="${DIST_DIR}/.OpenMW-Enhanced-x86_64.AppImage.pending"

"${LINUXDEPLOY}" \
    --appdir "${APPDIR}" \
    --desktop-file "${APPDIR}/usr/share/applications/openmw-enhanced.desktop" \
    --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/openmw-enhanced.png" \
    --executable "${APPDIR}/usr/bin/openmw-launcher" \
    --executable "${APPDIR}/usr/bin/openmw" \
    "${osg_dependency_args[@]}" \
    --deploy-deps-only "${OSG_PLUGIN_DEST}" \
    --plugin qt \
    --output appimage

for plugin in "${osg_plugins[@]}"; do
    flat_plugin="${APPDIR}/usr/lib/osgdb_${plugin}.so"
    canonical_plugin="${OSG_PLUGIN_DEST}/osgdb_${plugin}.so"
    if [[ ! -f "${canonical_plugin}" ]]; then
        echo "Missing canonical OSG plugin after deployment: ${canonical_plugin}" >&2
        exit 1
    fi
    if [[ -e "${flat_plugin}" || -L "${flat_plugin}" ]]; then
        if [[ ! "${flat_plugin}" -ef "${canonical_plugin}" ]]; then
            echo "Duplicate physical OSG plugin after deployment: ${flat_plugin}" >&2
            exit 1
        fi
    fi
done
for library in "${osg_libraries[@]}"; do
    library_name="$(basename -- "${library}")"
    if [[ ! "${APPDIR}/usr/lib/${library_name%.${OSG_ABI}}" -ef "${APPDIR}/usr/lib/${library_name}" ]]; then
        echo "Missing or incorrect bundled OSG node-kit alias: ${library_name%.${OSG_ABI}}" >&2
        exit 1
    fi
done
for binary in "${OSG_PLUGIN_DEST}"/osgdb_*.so "${APPDIR}"/usr/lib/libosg*.so."${OSG_ABI}"; do
    check_osg_dependencies "${binary}"
done
if [[ ! "${APPDIR}/usr/lib/openmw-osg-plugins" -ef "${OSG_PLUGIN_DEST}" ]]; then
    echo "OSG plugin alias no longer points to the build's plugin directory." >&2
    exit 1
fi
mv -f -- "${OUTPUT}" "${DIST_DIR}/OpenMW-Enhanced-x86_64.AppImage"
OUTPUT="${DIST_DIR}/OpenMW-Enhanced-x86_64.AppImage"

echo "AppImage written to ${OUTPUT}"
