#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

# Required CI must not certify a partial matrix. Local developers may leave
# this unset to run only the compilers installed on their workstation.
if [ "${ORES_SCOPED_LOCAL_REQUIRE_FULL_MATRIX:-0}" = "1" ]; then
  for compiler in gcc g++ clang clang++; do
    if ! command -v "$compiler" >/dev/null 2>&1; then
      echo "missing required native scoped-local compiler: $compiler" >&2
      exit 1
    fi
  done
fi
found=0
for CXX in g++ clang++; do
  if ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
  found=1
  "$CXX" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" "$ROOT/tests/native_scoped_local_storage_v1_test.cpp" \
    -o "$OUT/scoped-locals-$CXX"
  "$OUT/scoped-locals-$CXX"
  "$CXX" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" "$ROOT/tests/native_actor_local_gate_v1_test.cpp" \
    -o "$OUT/native-actor-local-gate-$CXX"
  "$OUT/native-actor-local-gate-$CXX"
  "$CXX" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" "$ROOT/tests/native_scoped_request_parity_v1_test.cpp" \
    -o "$OUT/native-scoped-request-parity-$CXX"
  "$OUT/native-scoped-request-parity-$CXX"
  "$CXX" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" "$ROOT/tests/native_request_mail_gate_v1_test.cpp" \
    -o "$OUT/native-request-mail-gate-$CXX"
  "$OUT/native-request-mail-gate-$CXX"
done
if [ "$found" -eq 0 ]; then echo 'No C++17 compiler available' >&2; exit 1; fi
# Compile-time scope confusion must fail, not reinterpret actor keys as TaskLocal.
cat > "$OUT/negative.cpp" <<'CPP'
#include "oreslang/runtime/ScopedLocalStorageV1.hpp"
using namespace oreslang::runtime;
int main() {
  TaskLocalStoreV1 t({1,2,3,4,5});
  ActorLocalKeyV1<int> actor_key;
  return t.set(actor_key, {1,2,3,4,5}, 1) ? 0 : 1;
}
CPP
for CXX in g++ clang++; do
  if ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
  if "$CXX" -std=c++17 -fsyntax-only -I"$ROOT/src/main/cpp" \
       "$OUT/negative.cpp" >"$OUT/neg.log" 2>&1; then
    echo "scope-confusion unexpectedly compiled with $CXX" >&2; exit 1
  fi
done
# Raw pointer element types cannot be used as scoped local keys.
cat > "$OUT/raw-pointer-negative.cpp" <<'CPP'
#include "oreslang/runtime/ScopedLocalStorageV1.hpp"
using namespace oreslang::runtime;
int main() {
  ActorLocalKeyV1<int*> unsafe;
  return unsafe.host_key_id_for_diagnostics() != 0;
}
CPP
for CXX in g++ clang++; do
  if ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
  if "$CXX" -std=c++17 -fsyntax-only -I"$ROOT/src/main/cpp" \
       "$OUT/raw-pointer-negative.cpp" >"$OUT/ptr-neg.log" 2>&1; then
    echo "raw-pointer scoped key unexpectedly compiled with $CXX" >&2; exit 1
  fi
done
# Cross-language native execution authority is included by the C++ actor gate:
# keep its original C11 ABI and real physical-claim tests working too.
for CC in gcc clang; do
  if ! command -v "$CC" >/dev/null 2>&1; then continue; fi
  "$CC" -std=c11 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" "$ROOT/tests/native_execution_authority_test.c" \
    -o "$OUT/execution-authority-$CC"
  "$OUT/execution-authority-$CC"
done

# Real native scheduler C11 integration. This is not a synthetic model:
# both the original-mail API and the DATA dispatcher are compiled/linked.
for CC in gcc clang; do
  if ! command -v "$CC" >/dev/null 2>&1; then continue; fi
  for testfile in native_original_data_mail_sequence_v1_test native_data_mail_stop_v1_test; do
    "$CC" -std=c11 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
      -I"$ROOT/src/main/cpp" \
      "$ROOT/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
      "$ROOT/tests/$testfile.c" \
      -o "$OUT/$testfile-$CC"
    "$OUT/$testfile-$CC"
  done
done

# C11 scheduler + C++ RequestLocal gate integration. Compile the REAL native
# scheduler source as C, not through a C++ shim (C ABI/C11 atomics matter).
for pair in gcc:g++ clang:clang++; do
  CC="${pair%%:*}"
  CXX="${pair#*:}"
  if ! command -v "$CC" >/dev/null 2>&1 || \
     ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
  "$CC" -std=c11 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -I"$ROOT/src/main/cpp" \
    -c "$ROOT/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
    -o "$OUT/native-scheduler-$CC.o"
  for testfile in native_request_data_mail_gate_v1_test native_request_wake_origin_v1_test; do
    "$CXX" -std=c++17 -O2 -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
      -I"$ROOT/src/main/cpp" \
      "$ROOT/tests/$testfile.cpp" \
      "$OUT/native-scheduler-$CC.o" \
      -o "$OUT/$testfile-$CXX"
    "$OUT/$testfile-$CXX"
  done
done

# Optional exact-source sanitizer matrix, enabled by the source-local workflow.
if [ "${ORES_SCOPED_LOCAL_RUN_SANITIZERS:-0}" = "1" ]; then
  for CC in gcc clang; do
    if ! command -v "$CC" >/dev/null 2>&1; then continue; fi
    for testfile in native_original_data_mail_sequence_v1_test native_data_mail_stop_v1_test; do
      "$CC" -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/src/main/cpp" \
        "$ROOT/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
        "$ROOT/tests/$testfile.c" \
        -o "$OUT/asan-$testfile-$CC"
      ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
        "$OUT/asan-$testfile-$CC"
    done
  done
  for pair in gcc:g++ clang:clang++; do
    CC="${pair%%:*}"
    CXX="${pair#*:}"
    if ! command -v "$CC" >/dev/null 2>&1 || \
       ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
    "$CC" -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
      -fsanitize=address,undefined -fno-omit-frame-pointer \
      -I"$ROOT/src/main/cpp" \
      -c "$ROOT/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
      -o "$OUT/asan-native-scheduler-$CC.o"
    for testfile in native_request_data_mail_gate_v1_test native_request_wake_origin_v1_test; do
      "$CXX" -std=c++17 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/src/main/cpp" \
        "$ROOT/tests/$testfile.cpp" \
        "$OUT/asan-native-scheduler-$CC.o" \
        -o "$OUT/asan-$testfile-$CXX"
      ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" \
        "$OUT/asan-$testfile-$CXX"
    done
  done
  for CXX in g++ clang++; do
    if ! command -v "$CXX" >/dev/null 2>&1; then continue; fi
    for testfile in native_scoped_local_storage_v1_test native_actor_local_gate_v1_test native_scoped_request_parity_v1_test native_request_mail_gate_v1_test; do
      "$CXX" -std=c++17 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$ROOT/src/main/cpp" "$ROOT/tests/$testfile.cpp" \
        -o "$OUT/asan-$testfile-$CXX"
      ASAN_OPTIONS="detect_leaks=1:abort_on_error=1" "$OUT/asan-$testfile-$CXX"
    done
  done
fi

# GCC ThreadSanitizer runs only on native scheduler-backed request
# provenance tests. The producer/actor WAKE race is not covered by ASan/UBSan.
if [ "${ORES_SCOPED_LOCAL_RUN_TSAN:-0}" = "1" ]; then
  if ! command -v gcc >/dev/null 2>&1 || \
     ! command -v g++ >/dev/null 2>&1; then
    echo "GCC/G++ required for native request TSan" >&2; exit 1
  fi
  gcc -std=c11 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
    -fsanitize=thread -fno-omit-frame-pointer \
    -I"$ROOT/src/main/cpp" \
    -c "$ROOT/src/main/cpp/oreslang/runtime/NativeActorScheduler.c" \
    -o "$OUT/tsan-native-scheduler.o"
  for testfile in native_request_data_mail_gate_v1_test native_request_wake_origin_v1_test; do
    g++ -std=c++17 -O1 -g -pthread -Wall -Wextra -Wpedantic -Werror -UNDEBUG \
      -fsanitize=thread -fno-omit-frame-pointer \
      -I"$ROOT/src/main/cpp" \
      "$ROOT/tests/$testfile.cpp" "$OUT/tsan-native-scheduler.o" \
      -o "$OUT/tsan-$testfile"
    TSAN_OPTIONS="halt_on_error=1" "$OUT/tsan-$testfile"
  done
fi

printf '%s\n' 'SCOPED_LOCAL_HOST_ONLY_COMPILER_AND_NEGATIVE_PASS'
