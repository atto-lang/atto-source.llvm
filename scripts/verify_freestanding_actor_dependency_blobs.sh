#!/usr/bin/env bash
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
expect() {
  local path="$1" sha="$2" actual
  actual="$(git hash-object "$path")"
  if [[ "$actual" != "$sha" ]]; then
    printf 'FREESTANDING_ABI_EXACT_DEPENDENCY_MISMATCH %s expected=%s actual=%s\n' "$path" "$sha" "$actual" >&2
    return 1
  fi
  printf 'FREESTANDING_ABI_EXACT_DEPENDENCY_PASS %s\n' "$path"
}
expect src/main/cpp/oreslang/runtime/FreestandingActorKernel.c 04dce7bcf708dbe0296a869f42e065bcfebd06c5
expect src/main/cpp/oreslang/runtime/FreestandingActorKernel.h 40415a683411931ee7c326ef609f4d84f4944aff
expect src/main/cpp/oreslang/runtime/SourceActorBootstrap.h 29d1b8afeca0ef9c4a596a3222a683c3f6195161
expect src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h dc30ed99bf6d4268fc15a841d751e97e9eefaa06
