#!/usr/bin/env bash
# Exact-current-source stage-0 native AOT compiler admission for std/time.
# SUCCESS of this script is NOT a guest parity PASS unless the machine-readable
# result says VERIFIED_GUEST_EXECUTED. Unsupported compiler syntax must be
# rejected without publishing an artifact. No host model/queue can replace it.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true
expected_head="$(git rev-parse HEAD)"
expected_tree="$(git rev-parse 'HEAD^{tree}')"
test -z "$(git status --porcelain)"
echo "ATTO_TIME_SOURCE_HEAD=$expected_head"
echo "ATTO_TIME_SOURCE_TREE=$expected_tree"
source=examples/atto-time-contract.atto
test -f "$source"
grep -Fxq 'import module time from "std/time";' "$source"
test "$(grep -c 'check(' "$source")" -eq 53
test "$(grep -c 'TIME-CONTRACT-DONE' "$source")" -eq 1
echo "ATTO_TIME_GUEST_SOURCE_BLOB=$(git hash-object "$source")"
cargo build --offline --release --bin atto-llvm
compiler=target/release/atto-llvm
test -x "$compiler"
echo "ATTO_TIME_COMPILER_SHA256=$(sha256sum "$compiler" | cut -d' ' -f1)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
candidate="$work/time_guest"
if ! "$compiler" "$source" -o "$candidate" >"$work/compile-stdout" 2>"$work/compile-stderr"; then
  # A strict native scalar-only compiler may truthfully reject this guest.
  # The error must be typed and no executable must have been published.
  test ! -e "$candidate" || {
    echo "FAIL std/time unsupported compiler published guest artifact" >&2
    exit 1
  }
  if ! grep -Eq 'E_[A-Z][A-Z0-9_]*' "$work/compile-stderr" "$work/compile-stdout"; then
    echo "FAIL std/time admission lacked a typed compiler diagnostic" >&2
    sed -n '1,35p' "$work/compile-stderr" >&2
    exit 1
  fi
  echo "STD_TIME_GUEST_UNSUPPORTED_NOT_PARITY"
  exit 0
fi
# Unexpected acceptance must prove genuine ELF executable and exact guest
# stdout. A compiler returning zero without output is ALWAYS a failure.
test -s "$candidate"
if command -v readelf >/dev/null; then
  readelf -h "$candidate" | grep -Eq 'Class:[[:space:]]*ELF64'
fi
echo "ATTO_TIME_GUEST_ARTIFACT_SHA256=$(sha256sum "$candidate" | cut -d' ' -f1)"
if ! timeout 20s "$candidate" >"$work/guest-stdout" 2>"$work/guest-stderr"; then
  echo "FAIL std/time guest execution" >&2
  sed -n '1,35p' "$work/guest-stderr" >&2
  exit 1
fi
python3 - "$work/guest-stdout" <<'PY'
import pathlib,sys
rows=pathlib.Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
assert len(rows)==53,(len(rows),rows[:3],rows[-3:])
assert rows[-1]=="TIME-CONTRACT-DONE",rows[-1]
labels=[x.removeprefix("TIME-PASS|") for x in rows[:-1]]
assert all(x.startswith("TIME-PASS|") for x in rows[:-1])
assert len(set(labels))==52,labels
for required in ("ceil-sub-ms","hours-overflow","manual-overflow-atomic",
                 "clock-mismatch","timer-no-repeat","interval-coalesce",
                 "deadline-overflow"):
    assert required in labels,required
print("STD_TIME_GUEST_VERIFIED_52_CASES")
PY
echo "STD_TIME_VERIFIED_GUEST_EXECUTED"
