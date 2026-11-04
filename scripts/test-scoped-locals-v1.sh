#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
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

# Optional exact-source sanitizer matrix, enabled by the source-local workflow.
if [ "${ORES_SCOPED_LOCAL_RUN_SANITIZERS:-0}" = "1" ]; then
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

printf '%s\n' 'SCOPED_LOCAL_HOST_ONLY_COMPILER_AND_NEGATIVE_PASS'
