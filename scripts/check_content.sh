#!/usr/bin/env bash
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_DIR"

forbidden_patterns=(
  '\.nup$'
  '\.keys$'
  '\.bin$'
  '\.dcx$'
  '\.bnd'
  'code_.*\.c$'
  'code_.*\.h$'
  '\.disasm$'
  'exefs/'
  'romfs/'
  'prod\.keys'
  'title\.keys'
)

failed=0
staged_files=$(git diff --cached --name-only 2>/dev/null || true)

if [[ -n "$staged_files" ]]; then
  for pat in "${forbidden_patterns[@]}"; do
    bad=$(echo "$staged_files" | grep -E "$pat" || true)
    if [[ -n "$bad" ]]; then
      echo "ERROR: Forbidden file staged: $bad (matches $pat)"
      failed=1
    fi
  done
fi

tracked_files=$(git ls-files 2>/dev/null || true)
if [[ -n "$tracked_files" ]]; then
  for pat in "${forbidden_patterns[@]}"; do
    bad=$(echo "$tracked_files" | grep -E "$pat" || true)
    if [[ -n "$bad" ]]; then
      echo "ERROR: Forbidden file tracked in git: $bad (matches $pat)"
      failed=1
    fi
  done
fi

if [[ $failed -ne 0 ]]; then
  echo "Content check failed. Remove forbidden files before staging."
  exit 1
fi

echo "Content check passed: no game data, keys, disassembly, shaders, or generated C found."
exit 0
