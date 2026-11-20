#!/usr/bin/env bash
# Run real Atto guest through native LLVM compilation; reject unsupported
# shapes before any LLVM IR. No host-only reference model is counted.
set -euo pipefail
if [[ $# -lt 2 || $# -gt 3 || (${3:-} != "" && ${3:-} != "--sanitize") ]]; then
  echo "usage: $0 /path/to/oreslang-llvmc /path/to/clang [--sanitize]" >&2
  exit 2
fi
compiler="$1"
clang="$2"
sanitized="no"
if [[ ${3:-} == "--sanitize" ]]; then sanitized="yes"; fi
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
fixtures="$repo_root/tests/mtd-receiver-v1"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

# Compile two distinct guest programs: a local class with receiver mtd and
# two separate actor identities with their own native on_message handlers.
# The native actor subset is not yet equivalent to Java's arbitrary actor mtd.
for guest in positive-local-class.atto positive-two-actors-forward.atto; do
  "$compiler" "$fixtures/$guest" > "$tmp/main.ll" 2> "$tmp/compile.err"
  if [[ ! -s "$tmp/main.ll" ]]; then
    echo "native receiver/actor fixture $guest generated no LLVM IR" >&2
    exit 1
  fi

  # Actor guests import the native scheduler/VM/actor-heap runtime; link the
  # same C sources scripts/compile-aot.sh links for source actors.
  runtime=()
  if grep -Eq 'declare .*@(oreslang_source_actor_|ores_actor_)' "$tmp/main.ll"; then
    rt="$repo_root/src/main/cpp/oreslang/runtime"
    runtime=(-x c -std=c11 -I "$repo_root/src/main/cpp"
      "$rt/ActorOwnedI64Heap.c" "$rt/NativeActorScheduler.c" "$rt/NativeOresVM.c"
      "$rt/NativeActorFuture.c" "$rt/SourceActorBootstrap.c"
      "$rt/NativeExecutionContextStore.c" "$rt/NativeContinuationStore.c"
      "$rt/NativeAwaitTraceBridge.c" "$rt/NativeLogicalTrace.c" -pthread)
  fi

  # Exercise both optimization and safety configurations.
  for opt in 0 2; do
    args=(-O"$opt")
    if [[ $sanitized == "yes" ]]; then
      args+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
    fi
    "$clang" "${args[@]}" -x ir "$tmp/main.ll" ${runtime[@]+"${runtime[@]}"} -o "$tmp/main-$opt"
    set +e
    "$tmp/main-$opt"
    code=$?
    set -e
    if [[ $code -ne 42 ]]; then
      echo "expected guest $guest exit=42 at O$opt sanitized=$sanitized, got=$code" >&2
      exit 1
    fi
  done
done

check_negative() {
  local file="$1" expected="$2" rc=0
  : > "$tmp/rejected.ll"
  : > "$tmp/rejected.err"
  "$compiler" "$fixtures/$file" > "$tmp/rejected.ll" 2> "$tmp/rejected.err" || rc=$?
  if [[ $rc -eq 0 || -s "$tmp/rejected.ll" ]] || ! grep -Fq "$expected" "$tmp/rejected.err"; then
    echo "FAIL $file: expected diagnostic '$expected', no executable IR" >&2
    sed -n '1,30p' "$tmp/rejected.err" >&2
    exit 1
  fi
}

check_negative negative-missing-receiver.atto NATIVE_MTD_LOCAL_CLASS_REQUIRED
check_negative negative-readonly-mutation.atto 'shared class borrow'
check_negative negative-duplicate-self.atto NATIVE_MTD_DUPLICATE_SELF
check_negative negative-duplicate-slot.atto NATIVE_MTD_DUPLICATE_SLOT
check_negative negative-lifecycle-spoof.atto NATIVE_MTD_RESERVED_MEMBER
check_negative negative-immutable-owner-call.atto immut
check_negative negative-actor-forward-missing.atto NATIVE_ACTOR_FORWARD_UNRESOLVED
check_negative negative-actor-forward-duplicate.atto NATIVE_ACTOR_FORWARD_DUPLICATE
check_negative negative-actor-forward-class.atto NATIVE_ACTOR_FORWARD_KIND
check_negative negative-actor-forward-namespace.atto NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED
check_negative negative-actor-forward-public.atto NATIVE_ACTOR_FORWARD_PUBLIC_UNSUPPORTED
check_negative negative-actor-forward-isoactor.atto NATIVE_ACTOR_FORWARD_KIND_UNSUPPORTED
check_negative negative-actor-forward-cross-module.atto NATIVE_ACTOR_FORWARD_SCOPE
check_negative negative-actor-forward-duplicate-definition.atto NATIVE_ACTOR_DUPLICATE_DEFINITION
check_negative negative-external-actor-mtd.atto NATIVE_MTD_ACTOR_STATE_SEALED
check_negative negative-declared-actor-type-first-mtd.atto NATIVE_ACTOR_MTD_BACKEND_UNSUPPORTED
echo "PASS: two native guest fixtures (O0/O2) and sixteen fail-closed cases; sanitized=$sanitized"
