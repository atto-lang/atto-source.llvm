#!/usr/bin/env bash
# Fail-closed receipt for EXACT private-source Git tree mirroring.
# Run BEFORE configure/build, with the source tree SHA from a trusted
# out-of-band GitHub connector read. No cross-org checkout secrets needed.
set -euo pipefail
if [[ "$#" -ne 1 || ! "${1:-}" =~ ^[0-9a-f]{40}$ ]]; then
  echo "usage: verify-exact-source-tree.sh <trusted-40-hex-source-tree-sha>" >&2
  exit 2
fi
expected="$1"
inside="$(git rev-parse --is-inside-work-tree 2>/dev/null || true)"
[[ "$inside" == "true" ]] || { echo "not a Git worktree" >&2; exit 2; }
actual="$(git rev-parse 'HEAD^{tree}')"
[[ "$actual" == "$expected" ]] || {
  echo "EXACT_TREE_DENIED expected=$expected actual=$actual" >&2
  exit 1
}
[[ -z "$(git status --porcelain --untracked-files=normal)" ]] || {
  echo "EXACT_TREE_DENIED checkout has tracked/untracked changes" >&2
  exit 1
}
git cat-file -e "$actual^{tree}"
echo "EXACT_TREE_VERIFIED source_tree=$expected mirror_commit=$(git rev-parse HEAD)"
