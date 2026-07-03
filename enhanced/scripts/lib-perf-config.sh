#!/usr/bin/env bash

enhanced_perf_init() {
    if [[ $# -ne 1 ]]; then
        echo "enhanced_perf_init requires a run name" >&2
        exit 2
    fi

    local script_dir
    script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

    OPENMW_ENHANCED_ROOT="$(cd -- "${script_dir}/../.." && pwd)"
    OPENMW_ENHANCED_APPIMAGE_PATH="${OPENMW_ENHANCED_APPIMAGE:-${OPENMW_ENHANCED_ROOT}/.build/dist/OpenMW-Enhanced-x86_64.AppImage}"
    OPENMW_ENHANCED_OUT_DIR="${OPENMW_ENHANCED_PERF_DIR:-${OPENMW_ENHANCED_ROOT}/.build/perf}"
    OPENMW_ENHANCED_RUN_NAME="$1"
    OPENMW_ENHANCED_RUN_DIR="${OPENMW_ENHANCED_OUT_DIR}/${OPENMW_ENHANCED_RUN_NAME}"
    OPENMW_ENHANCED_CONFIG_DIR="${OPENMW_ENHANCED_RUN_DIR}/config"
    OPENMW_ENHANCED_CONFIG_FILE="${OPENMW_ENHANCED_CONFIG_DIR}/openmw-enhanced.cfg"
    OPENMW_ENHANCED_CSV_FILE="${OPENMW_ENHANCED_RUN_DIR}/${OPENMW_ENHANCED_RUN_NAME}.csv"
    OPENMW_ENHANCED_LOG_FILE="${OPENMW_ENHANCED_RUN_DIR}/${OPENMW_ENHANCED_RUN_NAME}.log"

    mkdir -p "${OPENMW_ENHANCED_CONFIG_DIR}"
}

enhanced_perf_write_config() {
    cat > "${OPENMW_ENHANCED_CONFIG_FILE}"
}

enhanced_perf_print_paths() {
    echo "Using enhanced config ${OPENMW_ENHANCED_CONFIG_FILE}" >&2
    echo "Writing performance CSV to ${OPENMW_ENHANCED_CSV_FILE}" >&2
}

enhanced_perf_exec() {
    enhanced_perf_print_paths
    export OPENMW_ENHANCED_CONFIG_FILE="${OPENMW_ENHANCED_CONFIG_FILE}"
    exec "${OPENMW_ENHANCED_APPIMAGE_PATH}" "$@"
}

enhanced_perf_exec_logged() {
    enhanced_perf_print_paths
    echo "Writing launcher log to ${OPENMW_ENHANCED_LOG_FILE}" >&2
    export OPENMW_ENHANCED_CONFIG_FILE="${OPENMW_ENHANCED_CONFIG_FILE}"
    "${OPENMW_ENHANCED_APPIMAGE_PATH}" "$@" 2>&1 \
        | tee "${OPENMW_ENHANCED_LOG_FILE}"
    exit "${PIPESTATUS[0]}"
}
