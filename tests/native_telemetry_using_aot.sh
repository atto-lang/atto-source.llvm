#!/usr/bin/env bash
set -euo pipefail
if [ "$#" -ne 3 ]; then
  echo "usage: native_telemetry_using_aot.sh <oreslang-llvmc> <clang> <source-root>" >&2
  exit 2
fi
COMPILER="$1"
CLANG="$2"
ROOT="$3"
test -x "$COMPILER" || { echo "native compiler not executable" >&2; exit 1; }
test -x "$CLANG" || { echo "Clang not executable" >&2; exit 1; }
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
RUNTIME="$ROOT/src/main/cpp/oreslang/runtime/NativeTelemetrySpan.c"
test -f "$RUNTIME" || { echo "native telemetry runtime not found" >&2; exit 1; }

# Host-only enable/proof hook; scope start/end remain linked from actual Atto
# compiler-generated LLVM IR. Never expose telemetry enable as guest authority.
cat > "$OUT/host-proof.c" <<'C'
#include "oreslang/runtime/NativeTelemetrySpan.h"
#include <stdlib.h>
#include <stdio.h>
static unsigned expected;
__attribute__((constructor)) static void before_guest(void) {
    const char* text = getenv("ORES_EXPECT_SPANS");
    expected = text ? (unsigned)strtoul(text, 0, 10) : 0;
    ores_native_telemetry_set_enabled(1);
}
__attribute__((destructor)) static void after_guest(void) {
    const OresNativeTelemetrySnapshot s = ores_native_telemetry_snapshot();
    if (s.started != expected || s.ended != expected
            || s.active != 0 || s.dropped != 0) {
        fprintf(stderr, "TRACE_LIFETIME_FAILURE started=%llu ended=%llu active=%llu dropped=%llu expected=%u\n",
            (unsigned long long)s.started, (unsigned long long)s.ended,
            (unsigned long long)s.active, (unsigned long long)s.dropped, expected);
        _Exit(201);
    }
    fprintf(stderr, "NATIVE_GUEST_SPAN_CLEANUP_PASS:%u\n", expected);
}
C

compile_and_run() {
  local name="$1" expected_exit="$2" expected_scopes="$3"
  "$COMPILER" "$OUT/$name.atto" > "$OUT/$name.ll" 2> "$OUT/$name.compiler.log"
  grep -q 'ores_native_telemetry_span_begin' "$OUT/$name.ll" ||
    { echo "LLVM missing actual span begin" >&2; exit 1; }
  grep -q 'ores_native_telemetry_span_end' "$OUT/$name.ll" ||
    { echo "LLVM missing actual span end" >&2; exit 1; }
  # Check both unoptimized and optimized compiler-generated native code.
  for OPT in 0 2; do
    "$CLANG" "-O$OPT" -I"$ROOT/src/main/cpp" -x ir "$OUT/$name.ll" \
      -x c "$RUNTIME" "$OUT/host-proof.c" -o "$OUT/$name-O$OPT"
    local observed=0
    ORES_EXPECT_SPANS="$expected_scopes" "$OUT/$name-O$OPT" \
      > "$OUT/$name-O$OPT.stdout" 2> "$OUT/$name-O$OPT.stderr" || observed=$?
    test "$observed" -eq "$expected_exit" ||
      { echo "$name -O$OPT returned $observed, expected $expected_exit" >&2; cat "$OUT/$name-O$OPT.stderr" >&2; exit 1; }
    test ! -s "$OUT/$name-O$OPT.stdout" ||
      { echo "unexpected native guest stdout" >&2; exit 1; }
    grep -qx "NATIVE_GUEST_SPAN_CLEANUP_PASS:$expected_scopes" "$OUT/$name-O$OPT.stderr" ||
      { echo "$name -O$OPT failed exact span lifecycle check" >&2; cat "$OUT/$name-O$OPT.stderr" >&2; exit 1; }
  done
}

cat > "$OUT/early.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("checkout") as span {
    return 42;
  }
  return 1;
}
ATTO
compile_and_run early 42 1

cat > "$OUT/nested.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("outer") as outer {
    using telemetry.span("inner") as inner {
      return 43;
    }
  }
  return 1;
}
ATTO
compile_and_run nested 43 2

cat > "$OUT/normal.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("checkout") as span {
  }
  return 44;
}
ATTO
compile_and_run normal 44 1

reject_source() {
  local name="$1"
  if "$COMPILER" "$OUT/$name.atto" > "$OUT/$name.ll" 2> "$OUT/$name.err"; then
    echo "unsafe native scope was accepted: $name" >&2; exit 1
  fi
  test ! -s "$OUT/$name.ll" ||
    { echo "partial LLVM emitted for rejected scope: $name" >&2; exit 1; }
  test -s "$OUT/$name.err" ||
    { echo "rejection missing diagnostic: $name" >&2; exit 1; }
}
cat > "$OUT/impersonation.atto" <<'ATTO'
pub fnc main(): int {
  using unrelated.span("checkout") as span { return 2; }
  return 3;
}
ATTO
reject_source impersonation
cat > "$OUT/nonliteral.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span(name) as span { return 2; }
  return 3;
}
ATTO
reject_source nonliteral
cat > "$OUT/escape.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("checkout") as span { return span; }
  return 3;
}
ATTO
reject_source escape
cat > "$OUT/forged-close.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("checkout") as span { span.close_scope(); }
  return 3;
}
ATTO
reject_source forged-close

# Enabled instrumentation is not yet admitted by hard-real-time WCET policy.
if "$COMPILER" --profile=native-hard-rt "$OUT/normal.atto" \
    > "$OUT/hard-rt.ll" 2> "$OUT/hard-rt.err"; then
  echo "unbudgeted native telemetry incorrectly certified as hard-real-time" >&2
  exit 1
fi
test ! -s "$OUT/hard-rt.ll" ||
  { echo "hard-real-time denial emitted partial LLVM IR" >&2; exit 1; }
grep -q "telemetry using lacks bounded WCET" "$OUT/hard-rt.err" ||
  { echo "hard-real-time telemetry denial did not reach WCET gate" >&2; cat "$OUT/hard-rt.err" >&2; exit 1; }

echo 'LLVM_AOT_GUEST_TELEMETRY_SCOPE_EXECUTION_PASS:3:5'
