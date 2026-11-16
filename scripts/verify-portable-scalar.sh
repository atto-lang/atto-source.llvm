#!/usr/bin/env bash
# No host-language arithmetic substitute: compile native Oreslang via LLVM and run the executable.
set -euo pipefail
export LC_ALL=C
die() { echo "LLVM_PORTABLE_SCALAR_NOT_ACCEPTED: $*" >&2; exit 1; }
cd "$(dirname "$0")/.."
[[ "${EXPECTED_SOURCE_SHA:-}" =~ ^[a-f0-9]{40}$ ]] || die "missing exact-head SHA"
[[ "$(git rev-parse HEAD)" == "$EXPECTED_SOURCE_SHA" ]] || die "checkout is not the expected head"
[[ -d tests/portable-scalar && ! -L tests/portable-scalar ]] || die "fixture directory invalid"
[[ -z "$(git status --porcelain --untracked-files=normal -- tests/portable-scalar)" ]] || die "dirty fixture checkout"
compiler="${ORESLANG_LLVMC:-$PWD/build/oreslang-llvmc}"
[[ -x "$compiler" && -f "$compiler" && ! -L "$compiler" ]] || die "native compiler unavailable"
[[ "${ORESLANG_LLVM_COMPILER_SHA256:-}" =~ ^[a-f0-9]{64}$ ]] || die "missing compiler SHA"
[[ "$(sha256sum "$compiler" | cut -d ' ' -f 1)" == "$ORESLANG_LLVM_COMPILER_SHA256" ]] || die "compiler SHA mismatch"
export ORESLANG_LLVMC="$compiler"
limiter="$(command -v timeout || command -v gtimeout || true)"
[[ -n "$limiter" ]] || die "bounded runner required"
tmp="$(mktemp -d)" || die "cannot allocate temporary workspace"
trap 'rm -rf "$tmp"' EXIT
chmod 700 "$tmp"
count=0
: > "$tmp/seen"
while IFS='|' read -r source oracle blob extra || [[ -n "$source" ]]; do
  [[ -z "$extra" && "$source" =~ ^tests/portable-scalar/[0-9]{2}-[a-z0-9-]+\.ores$ ]] || die "malformed fixture row"
  [[ "$oracle" == 42 && "$blob" =~ ^[a-f0-9]{40}$ ]] || die "invalid oracle or Git blob"
  [[ -f "$source" && ! -L "$source" ]] || die "nonregular fixture"
  git ls-files --error-unmatch -- "$source" >/dev/null || die "untracked source"
  ! grep -Fqx "$source" "$tmp/seen" || die "duplicate fixture"
  printf '%s\n' "$source" >> "$tmp/seen"
  ((++count <= 6)) || die "too many fixtures"
  [[ "$(git rev-parse "HEAD:$source")" == "$blob" ]] || die "committed source blob mismatch"
  [[ "$(git hash-object -- "$source")" == "$blob" ]] || die "working fixture differs from reviewed Git"
  # The compiler generates LLVM IR and Clang links it to a new native binary.
  "$limiter" -k 5s 45s bash scripts/compile-aot.sh "$source" "$tmp/guest" >"$tmp/compile.stdout" 2>"$tmp/compile.stderr" || {
    cat "$tmp/compile.stderr" >&2
    die "LLVM/Clang compilation failed for $source"
  }
  [[ -x "$tmp/guest" && -s "$tmp/guest" ]] || die "no native executable published"
  [[ ! -s "$tmp/compile.stderr" ]] || die "unexpected compiler stderr"
  set +e
  "$limiter" -k 3s 20s "$tmp/guest" >"$tmp/guest.stdout" 2>"$tmp/guest.stderr"
  code=$?
  set -e
  [[ "$code" -eq "$oracle" ]] || die "native guest returned $code, expected $oracle: $source"
  [[ ! -s "$tmp/guest.stdout" && ! -s "$tmp/guest.stderr" ]] || die "unexpected guest output"
  [[ "$(git hash-object -- "$source")" == "$blob" ]] || die "guest source mutated during compilation"
  [[ "$(sha256sum "$compiler" | cut -d ' ' -f 1)" == "$ORESLANG_LLVM_COMPILER_SHA256" ]] || die "compiler bytes changed"
  printf 'LLVM_PORTABLE_SCALAR_PASS source=%s blob=%s artifact_sha256=%s\n' "$source" "$blob" "$(sha256sum "$tmp/guest" | cut -d ' ' -f 1)"
done < tests/portable-scalar/oracles.tsv
[[ "$count" -eq 6 ]] || die "missing portable scalar cases"
[[ "$(git ls-files 'tests/portable-scalar/*.ores' | wc -l | tr -d ' ')" == 6 ]] || die "unmanifested fixtures"
[[ -z "$(git status --porcelain --untracked-files=normal -- tests/portable-scalar)" ]] || die "fixture checkout was modified"
printf 'LLVM_PORTABLE_SCALAR_ACCEPTED source_sha=%s compiler_sha256=%s cases=6\n' "$EXPECTED_SOURCE_SHA" "$ORESLANG_LLVM_COMPILER_SHA256"
