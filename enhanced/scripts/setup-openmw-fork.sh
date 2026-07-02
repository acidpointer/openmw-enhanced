#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"

usage() {
    cat <<'EOF'
Usage: ./enhanced/scripts/setup-openmw-fork.sh [--fork-url URL] [--branch NAME]

Configure this repository as the actual OpenMW fork:
  upstream = canonical OpenMW repository, fetch only
  origin   = your fork repository, when --fork-url or OPENMW_FORK_URL is set

Environment:
  OPENMW_UPSTREAM_URL Upstream Git URL. Default: https://gitlab.com/OpenMW/openmw.git.
  OPENMW_FORK_URL     Fork Git URL used for origin.
  OPENMW_FORK_BRANCH  Local fork branch. Default: enhanced/main.
EOF
}

FORK_URL="${OPENMW_FORK_URL:-}"
FORK_BRANCH="${OPENMW_FORK_BRANCH:-enhanced/main}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --fork-url)
            if [[ $# -lt 2 ]]; then
                echo "--fork-url requires a URL" >&2
                exit 2
            fi
            FORK_URL="$2"
            shift 2
            ;;
        --fork-url=*)
            FORK_URL="${1#*=}"
            shift
            ;;
        --branch)
            if [[ $# -lt 2 ]]; then
                echo "--branch requires a branch name" >&2
                exit 2
            fi
            FORK_BRANCH="$2"
            shift 2
            ;;
        --branch=*)
            FORK_BRANCH="${1#*=}"
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
done

UPSTREAM_URL="${OPENMW_UPSTREAM_URL:-https://gitlab.com/OpenMW/openmw.git}"

if ! git -C "${ROOT}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    echo "${ROOT} is not a Git repository" >&2
    exit 1
fi

if git -C "${ROOT}" remote get-url upstream >/dev/null 2>&1; then
    git -C "${ROOT}" remote set-url upstream "${UPSTREAM_URL}"
else
    git -C "${ROOT}" remote add upstream "${UPSTREAM_URL}"
fi
git -C "${ROOT}" remote set-url --push upstream DISABLED

if [[ -n "${FORK_URL}" ]]; then
    if git -C "${ROOT}" remote get-url origin >/dev/null 2>&1; then
        git -C "${ROOT}" remote set-url origin "${FORK_URL}"
    else
        git -C "${ROOT}" remote add origin "${FORK_URL}"
    fi
fi

if ! git -C "${ROOT}" show-ref --verify --quiet "refs/heads/${FORK_BRANCH}"; then
    git -C "${ROOT}" switch -c "${FORK_BRANCH}"
else
    git -C "${ROOT}" switch "${FORK_BRANCH}"
fi

cat <<EOF
OpenMW fork repository configured at ${ROOT}

Remotes:
$(git -C "${ROOT}" remote -v)

Current branch:
$(git -C "${ROOT}" branch --show-current)
EOF

if ! git -C "${ROOT}" remote get-url origin >/dev/null 2>&1; then
    cat <<'EOF'

No fork remote is configured yet.
Create your fork, then run:
  ./enhanced/scripts/setup-openmw-fork.sh --fork-url <your-fork-url>
  git push -u origin enhanced/main
EOF
fi
