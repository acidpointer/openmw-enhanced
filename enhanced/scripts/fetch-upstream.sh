#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/fetch-upstream.sh

Compatibility wrapper for the old bootstrap command.
Prefer:
  ./enhanced/scripts/setup-openmw-fork.sh
  ./enhanced/scripts/sync-openmw-upstream.sh

Environment:
  OPENMW_UPSTREAM_URL Upstream Git URL. Default: https://gitlab.com/OpenMW/openmw.git.
  OPENMW_UPSTREAM_REF Upstream branch/ref. Default: master.
EOF
}

case "${1:-}" in
    -h|--help)
        usage
        exit 0
        ;;
    "")
        ;;
    *)
        echo "Unknown argument: $1" >&2
        usage >&2
        exit 2
        ;;
esac

"${ROOT}/enhanced/scripts/setup-openmw-fork.sh"
"${ROOT}/enhanced/scripts/sync-openmw-upstream.sh"
