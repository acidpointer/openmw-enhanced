#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/lib-perf-config.sh"

enhanced_perf_init "screen-space-shadows"
enhanced_perf_write_config <<EOF
[Performance]
gpu profile = true
scene profile = true
camera profile = true
gpu profile csv = ${OPENMW_ENHANCED_CSV_FILE}

[Shadows]
screen space shadows = true
screen space shadows force postprocess = true
EOF

echo "Screen-space shadows enabled; tune shader parameters in the post-processing HUD." >&2
enhanced_perf_exec_logged "$@"
