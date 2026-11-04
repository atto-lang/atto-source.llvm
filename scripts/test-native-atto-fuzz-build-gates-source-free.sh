#!/usr/bin/env bash
# SOURCE-FREE build-gate canary: synthetic Clang LLVM IR, no AttoLang frontend.
# Checks the exact native fuzz builder's timeout/ABI/auditor/publication paths.
set -euo pipefail
(( $# == 1 )) || { echo 'usage: test-native-atto-fuzz-build-gates-source-free.sh CLANG' >&2; exit 64; }
clang="$1"
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
builder="$root/scripts/build-native-atto-fuzz-target.sh"
[[ -f "$builder" ]] || { echo 'missing native fuzz builder' >&2; exit 69; }
if command -v timeout >/dev/null 2>&1; then
  timeout_cmd="$(command -v timeout)"
elif command -v gtimeout >/dev/null 2>&1; then
  timeout_cmd="$(command -v gtimeout)"
else
  echo 'GNU timeout required to exercise fail-closed builder budget' >&2
  exit 69
fi
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
printf '// synthetic source-free build input\n' > "$work/dummy.atto"
cat > "$work/good.c" <<'GUEST'
#include <stdint.h>
int64_t fuzz_byte(int64_t value) { return value + 1; }
GUEST
"$clang" -S -emit-llvm -O0 "$work/good.c" -o "$work/good.ll"
cat > "$work/compiler" <<'COMPILER'
#!/usr/bin/env bash
cat "$ATTO_SOURCE_FREE_IR"
COMPILER
cat > "$work/auditor" <<'AUDITOR'
#!/usr/bin/env bash
exit 0
AUDITOR
cat > "$work/hung" <<'HUNG'
#!/usr/bin/env bash
sleep 30
HUNG
chmod 700 "$work/compiler" "$work/auditor" "$work/hung"
export ATTO_SOURCE_FREE_IR="$work/good.ll"
export ORESLANG_LLVMC="$work/compiler"
export ORES_NO_GC_IR_AUDIT="$work/auditor"
export CLANG="$clang"
target="$work/target"
# This passes synthetic LLVM IR through the exact builder, never the guest
# compiler. A green result only proves the builder accepts a sound IR ABI.
bash "$builder" "$work/dummy.atto" "$target" >"$work/out" 2>"$work/err"
[[ -x "$target" && ! -s "$work/err" ]] ||
  { echo 'synthetic LLVM IR native builder control failed' >&2; exit 1; }
printf '\x00\xff' | "$target" >"$work/guest-out" 2>"$work/guest-err"
[[ ! -s "$work/guest-out" && ! -s "$work/guest-err" ]] ||
  { echo 'source-free built target did not satisfy native scalar oracle' >&2; exit 1; }
cp "$target" "$work/preserved"

# An invalid timeout may not launch ANY compiler, even one that could work.
for bad in 0 00 01 121 999 -1 1s abc; do
  set +e
  ATTO_FUZZ_BUILD_TIMEOUT_SECONDS="$bad" bash "$builder" \
    "$work/dummy.atto" "$target" >"$work/out" 2>"$work/err"
  rc=$?
  set -e
  [[ "$rc" == 64 ]] ||
    { echo "builder admitted invalid timeout value $bad (rc=$rc)" >&2; exit 1; }
  cmp -s "$target" "$work/preserved" ||
    { echo "invalid timeout overwrote preserved executable ($bad)" >&2; exit 1; }
done

# Outer independent watchdog is a fixture safety net; an *internal* one-second
# timeout must fire well before the outer ten-second watchdog.
for component in compiler auditor clang; do
  export ORESLANG_LLVMC="$work/compiler"
  export ORES_NO_GC_IR_AUDIT="$work/auditor"
  export CLANG="$clang"
  case "$component" in
    compiler) export ORESLANG_LLVMC="$work/hung";;
    auditor) export ORES_NO_GC_IR_AUDIT="$work/hung";;
    clang) export CLANG="$work/hung";;
  esac
  SECONDS=0
  set +e
  ATTO_FUZZ_BUILD_TIMEOUT_SECONDS=1 "$timeout_cmd" --kill-after=2s 10s \
    bash "$builder" "$work/dummy.atto" "$target" >"$work/out" 2>"$work/err"
  rc=$?
  set -e
  [[ "$rc" == 124 && "$SECONDS" -lt 6 ]] ||
    { echo "builder $component child did not fail closed at internal 1s timeout (rc=$rc duration=$SECONDS)" >&2; exit 1; }
  cmp -s "$target" "$work/preserved" ||
    { echo "timed-out $component process overwrote executable" >&2; exit 1; }
done
printf 'ATTO_LLVM_SOURCE_FREE_FUZZ_BUILD_GATES_PASS timeout_mutations=11 original_preserved=1\n'
