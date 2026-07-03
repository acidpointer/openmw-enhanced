#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib-perf-config.sh"

enhanced_perf_init "wide"
enhanced_perf_write_config <<EOF
[Performance]
gpu profile = true
gpu profile csv = ${OPENMW_ENHANCED_CSV_FILE}

[Renderer]
transparent depth mode = alpha-test-only

[Occlusion]
occlusion water cameras = water
EOF

enhanced_perf_exec "$@"
