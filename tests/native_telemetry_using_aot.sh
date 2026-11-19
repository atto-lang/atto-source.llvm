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
    const char* enable = getenv("ORES_TEST_TRACING_ON");
    ores_native_telemetry_set_enabled(enable && enable[0] == '1');
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
  if ! "$COMPILER" "$OUT/$name.atto" > "$OUT/$name.ll" 2> "$OUT/$name.compiler.log"; then
    echo "Atto compiler rejected $name before native executable" >&2
    cat "$OUT/$name.compiler.log" >&2
    exit 1
  fi
  grep -q 'ores_native_telemetry_span_begin' "$OUT/$name.ll" ||
    { echo "LLVM missing actual span begin" >&2; exit 1; }
  grep -q 'ores_native_telemetry_span_end' "$OUT/$name.ll" ||
    { echo "LLVM missing actual span end" >&2; exit 1; }
  # Check both unoptimized and optimized compiler-generated native code.
  for OPT in 0 2; do
    "$CLANG" "-O$OPT" -I"$ROOT/src/main/cpp" -x ir "$OUT/$name.ll" \
      -x c "$RUNTIME" "$OUT/host-proof.c" -o "$OUT/$name-O$OPT"
    # Both enabled and disabled recording must preserve the guest OS exit,
    # including the same emitted probe sites and no event when disabled.
    for mode in on off; do
      local count="$expected_scopes" enabled=1
      if [[ "$mode" == off ]]; then count=0; enabled=0; fi
      local observed=0
      ORES_TEST_TRACING_ON="$enabled" ORES_EXPECT_SPANS="$count" "$OUT/$name-O$OPT" \
        > "$OUT/$name-O$OPT-$mode.stdout" 2> "$OUT/$name-O$OPT-$mode.stderr" || observed=$?
      test "$observed" -eq "$expected_exit" ||
        { echo "$name -O$OPT $mode returned $observed, expected $expected_exit" >&2; cat "$OUT/$name-O$OPT-$mode.stderr" >&2; exit 1; }
      test ! -s "$OUT/$name-O$OPT-$mode.stdout" ||
        { echo "unexpected native guest stdout for $name -O$OPT $mode" >&2; exit 1; }
      grep -qx "NATIVE_GUEST_SPAN_CLEANUP_PASS:$count" "$OUT/$name-O$OPT-$mode.stderr" ||
        { echo "$name -O$OPT $mode failed exact span lifetime check" >&2; cat "$OUT/$name-O$OPT-$mode.stderr" >&2; exit 1; }
    done
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

cat > "$OUT/conditional-true.atto" <<'ATTO'
pub fnc main(): int {
  if true {
    using telemetry.span("true-branch") as chosen {
      return 45;
    }
  } else {
  } fi
  return 1;
}
ATTO
compile_and_run conditional-true 45 1

cat > "$OUT/conditional-false.atto" <<'ATTO'
pub fnc main(): int {
  if false {
    using telemetry.span("not-taken") as unentered {
      return 12;
    }
  } else {
  } fi
  return 46;
}
ATTO
compile_and_run conditional-false 46 0

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

# Java's continuation scope may outlive a physical carrier. This native AOT
# implementation currently has a carrier-local, strict-LIFO span stack and
# MUST NOT accept a suspension/yield inside it. Such a source requires a
# scheduler-owned scope frame, generation validation, and terminal cleanup.
# Each rejection must produce neither a native executable nor partial LLVM IR.
cat > "$OUT/scope-await.atto" <<'ATTO'
pub suspends fnc main(): int {
  using telemetry.span("await-boundary") as span {
    let int result = await next_value();
    return result;
  }
  return 1;
}
ATTO
reject_source scope-await

cat > "$OUT/scope-cooperate.atto" <<'ATTO'
pub fnc main(): int {
  using telemetry.span("cooperative-yield") as span {
    rt cooperate;
    return 41;
  }
  return 1;
}
ATTO
reject_source scope-cooperate

cat > "$OUT/scope-nested-await.atto" <<'ATTO'
pub suspends fnc main(): int {
  using telemetry.span("outer") as outer {
    using telemetry.span("inner") as inner {
      return await nested_result();
    }
  }
  return 1;
}
ATTO
reject_source scope-nested-await

echo 'LLVM_TELEMETRY_SUSPENSION_NEGATIVES_PASS:3'

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

echo 'LLVM_AOT_GUEST_TELEMETRY_SCOPE_EXECUTION_PASS:5:5'
