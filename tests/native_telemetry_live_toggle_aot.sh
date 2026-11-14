#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -ne 3 ]; then
  echo "usage: native_telemetry_live_toggle_aot.sh <oreslang-llvmc> <clang> <source-root>" >&2
  exit 2
fi
COMPILER="$1"
CLANG="$2"
ROOT="$3"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Deliberately no Atto 'main' function. This exported guest operation is
# invoked repeatedly by the C host and cannot access the privileged enable
# API. The same native process, function address and binary are used for
# each runtime transition. No JIT, code rewrite or recompilation occurs.
cat > "$OUT/toggle.atto" <<'ATTO'
pub fnc trace_once(): int {
  using telemetry.span("host-toggle") as scope {
    return 51;
  }
  return 0;
}
ATTO

if ! "$COMPILER" "$OUT/toggle.atto" > "$OUT/toggle.ll" 2> "$OUT/compiler.err"; then
  cat "$OUT/compiler.err" >&2
  echo "Atto source->LLVM compilation failed" >&2
  exit 1
fi
grep -Eq 'define (dso_local )?i64 @trace_once\(' "$OUT/toggle.ll" ||
  { echo "no reusable exported Atto trace_once symbol" >&2; exit 1; }
grep -q 'ores_native_telemetry_span_begin' "$OUT/toggle.ll"
grep -q 'ores_native_telemetry_span_end' "$OUT/toggle.ll"

cat > "$OUT/host-toggle.c" <<'C'
#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

extern int64_t trace_once(void);

static void check(uint64_t started, uint64_t ended) {
    OresNativeTelemetrySnapshot s = ores_native_telemetry_snapshot();
    assert(s.started == started);
    assert(s.ended == ended);
    assert(s.active == 0);
    assert(s.dropped == 0);
}
int main(void) {
    ores_native_telemetry_set_enabled(0);
    check(0, 0);
    assert(trace_once() == 51);
    check(0, 0);

    ores_native_telemetry_set_enabled(1);
    assert(trace_once() == 51);
    check(1, 1);

    ores_native_telemetry_set_enabled(0);
    assert(trace_once() == 51);
    check(1, 1);

    ores_native_telemetry_set_enabled(1);
    assert(trace_once() == 51);
    check(2, 2);

    puts("ATTO_AOT_SAME_PROCESS_DORMANT_PROBE_TOGGLE_PASS:4:2");
    return 0;
}
C
for OPT in 0 2; do
  # Suppress only Clang's expected IR target-triple override diagnostic:
  # native hosted AOT uses the runner's explicit host target at link time.
  "$CLANG" "-O$OPT" -Wall -Wextra -Wpedantic -Werror -Wno-override-module -UNDEBUG \
    -I"$ROOT/src/main/cpp" -x ir "$OUT/toggle.ll" \
    -x c "$ROOT/src/main/cpp/oreslang/runtime/NativeTelemetrySpan.c" \
    "$OUT/host-toggle.c" -o "$OUT/toggle-O$OPT"
  "$OUT/toggle-O$OPT" > "$OUT/toggle-O$OPT.stdout" 2> "$OUT/toggle-O$OPT.stderr" ||
    { cat "$OUT/toggle-O$OPT.stderr" >&2; exit 1; }
  grep -qx 'ATTO_AOT_SAME_PROCESS_DORMANT_PROBE_TOGGLE_PASS:4:2' "$OUT/toggle-O$OPT.stdout" ||
    { echo "missing exact same-process AOT live toggle receipt at -O$OPT" >&2; exit 1; }
  test ! -s "$OUT/toggle-O$OPT.stderr"
done
echo "LLVM_AOT_LIVE_PROBE_TOGGLE_EXECUTION_PASS:O0:O2"
