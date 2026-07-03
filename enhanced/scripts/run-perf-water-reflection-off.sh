#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib-perf-config.sh"

enhanced_perf_init "water-reflection-off"
enhanced_perf_write_config <<EOF
[Performance]
gpu profile = true
scene profile = true
gpu profile csv = ${OPENMW_ENHANCED_CSV_FILE}

[Water]
reflection = false
EOF

enhanced_perf_exec "$@"
