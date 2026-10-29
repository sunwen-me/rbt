#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
release_python="$(printenv RBT_RELEASE_PYTHON 2>/dev/null || true)"
if [[ -z "$release_python" ]]; then
    release_python=python3
fi
exec "$release_python" "$repo_root/packaging/package_release.py" "$@"
