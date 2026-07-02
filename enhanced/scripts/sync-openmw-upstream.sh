#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/sync-openmw-upstream.sh [--merge | --rebase]

Fetch OpenMW upstream and optionally update the current fork branch.

Default behavior fetches upstream only. This is intentionally non-destructive.

Options:
  --merge   Merge upstream/master into the current branch.
  --rebase  Rebase the current branch onto upstream/master.
  -h, --help

Environment:
  OPENMW_UPSTREAM_REF Upstream ref. Default: master.
EOF
}

MODE="fetch"

case "${1:-}" in
    --merge)
        MODE="merge"
        ;;
    --rebase)
        MODE="rebase"
        ;;
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

UPSTREAM_REF="${OPENMW_UPSTREAM_REF:-master}"

if ! git -C "${ROOT}" remote get-url upstream >/dev/null 2>&1; then
    echo "Missing upstream remote. Run ./enhanced/scripts/setup-openmw-fork.sh first." >&2
    exit 1
fi

if [[ -n "$(git -C "${ROOT}" status --porcelain)" ]]; then
    echo "Working tree has uncommitted changes. Commit or stash them before syncing upstream." >&2
    exit 1
fi

git -C "${ROOT}" fetch upstream "${UPSTREAM_REF}"

case "${MODE}" in
    fetch)
        echo "Fetched upstream/${UPSTREAM_REF}. No branch was changed."
        ;;
    merge)
        git -C "${ROOT}" merge "upstream/${UPSTREAM_REF}"
        ;;
    rebase)
        git -C "${ROOT}" rebase "upstream/${UPSTREAM_REF}"
        ;;
esac
