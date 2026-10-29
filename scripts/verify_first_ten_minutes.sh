#!/usr/bin/env bash
set -euo pipefail

# The onboarding scenario is intentionally the same scenario as the DX quick
# gate.  Keep this name for documentation and CI users who want to express the
# user journey directly.
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
    '') exec "${repo_root}/scripts/verify_dx.sh" --quick ;;
    --help|-h) exec "${repo_root}/scripts/verify_dx.sh" --help ;;
    --quick) exec "${repo_root}/scripts/verify_dx.sh" --quick ;;
    *)
        printf 'usage: scripts/verify_first_ten_minutes.sh [--quick|--help]\n' >&2
        exit 2
        ;;
esac
