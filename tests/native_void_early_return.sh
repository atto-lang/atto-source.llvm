#!/usr/bin/env bash
# Java parity: in a void function or method, `if c then return; fi` (and the
# `; do` / else forms) is an ordinary conditional whose arm returns early.
# run-*.atto: line 1 `// exit: N`, line 2 `// stdout: a|b|...` (lines joined
# by '|'); each is compiled, linked with the native String runtime and run at
# O0 and O2 through a pipe. reject-*.atto: line 1 `// expect: <diagnostic>`;
# compilation must fail without IR.
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fixtures="$here/fixtures/void-early-return-v1"
root="$(cd "$here/.." && pwd -P)"
string_runtime="$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
test -f "$string_runtime" || { echo "missing native String runtime" >&2; exit 2; }
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-void-return.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for executable in "$compiler" "$clang"; do
  [[ -x "$executable" ]] || { echo "not executable: $executable" >&2; exit 2; }
done
ran=0
rejected=0
for source in "$fixtures"/run-*.atto; do
  name="$(basename "$source" .atto)"
  expected_status="$(sed -n '1s|^// exit: ||p' "$source")"
  expected_stdout="$(sed -n '2s|^// stdout: ||p' "$source")"
  [[ -n "$expected_status" ]] || { echo "$name lacks // exit: line" >&2; exit 2; }
  ir="$tmp/$name.ll"
  if ! "$compiler" --target=host "$source" >"$ir" 2>"$tmp/$name.stderr"; then
    echo "compiler rejected void early-return fixture $name:" >&2
    cat "$tmp/$name.stderr" >&2
    exit 1
  fi
  test -s "$ir" || { echo "empty IR for $name" >&2; exit 1; }
  for opt in 0 2; do
    binary="$tmp/$name-O$opt"
    "$clang" "-O$opt" -w -x ir "$ir" -x c -std=c11 -I "$root/src/main/cpp" \
      "$string_runtime" -pthread -o "$binary"
    status=0
    # Native stdio admits pipes and sockets, not a redirected regular file;
    # with pipefail the pipeline status is the guest status (cat exits 0).
    "$binary" 2>"$tmp/$name-O$opt.stderr" | cat >"$tmp/$name-O$opt.stdout" || status=$?
    actual_stdout="$(paste -sd'|' - <"$tmp/$name-O$opt.stdout")"
    if [[ "$status" -ne "$expected_status" || "$actual_stdout" != "$expected_stdout" ]]; then
      echo "$name at O$opt: status=$status stdout='$actual_stdout'," \
        "expected status=$expected_status stdout='$expected_stdout'" >&2
      cat "$tmp/$name-O$opt.stderr" >&2
      exit 1
    fi
  done
  ran=$((ran + 1))
done
for source in "$fixtures"/reject-*.atto; do
  name="$(basename "$source" .atto)"
  expected="$(sed -n '1s|^// expect: ||p' "$source")"
  [[ -n "$expected" ]] || { echo "$name lacks // expect: line" >&2; exit 2; }
  out="$tmp/$name.ll"
  status=0
  "$compiler" --target=host "$source" >"$out" 2>"$tmp/$name.stderr" || status=$?
  if [[ "$status" -eq 0 ]]; then
    echo "compiler accepted invalid return fixture $name" >&2
    exit 1
  fi
  if [[ -s "$out" ]]; then
    echo "compiler emitted IR after rejecting $name" >&2
    exit 1
  fi
  if ! grep -Fq -- "$expected" "$tmp/$name.stderr"; then
    echo "$name: missing diagnostic '$expected'; got:" >&2
    cat "$tmp/$name.stderr" >&2
    exit 1
  fi
  rejected=$((rejected + 1))
done
test "$ran" -eq 5
test "$rejected" -eq 2
echo "void early return: $ran native O0/O2 guests + $rejected compile negatives passed"
