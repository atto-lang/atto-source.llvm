#!/usr/bin/env bash
# Source-local host-only native C proof. NOT compiled AttoLang guest evidence.
# Run inside the authorized source checkout, never export private source.
set -Eeuo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
for tool in git gcc clang timeout sha256sum grep mktemp; do
    command -v "$tool" >/dev/null || {
        echo "HOST_ONLY_NOT_EXECUTED: missing $tool" >&2; exit 69;
    }
done
test "$(git -C "$root" rev-parse --show-toplevel)" = "$root"
test -z "$(git -C "$root" status --porcelain --untracked-files=no)"
for path in \
    src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c \
    src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h \
    tests/actor_owned_i64_exclusive_loan.c; do
    test "$(git -C "$root" hash-object -- "$root/$path")" = \
         "$(git -C "$root" rev-parse "HEAD:$path")" || {
        echo "HOST_ONLY_NOT_EXECUTED: dirty source $path" >&2; exit 65;
    }
    printf 'HOST_ONLY_SOURCE_BLOB=%s:%s\n' "$path" "$(git -C "$root" rev-parse "HEAD:$path")"
done
printf 'HOST_ONLY_SOURCE_HEAD=%s\n' "$(git -C "$root" rev-parse HEAD)"
printf 'HOST_ONLY_SOURCE_TREE=%s\n' "$(git -C "$root" rev-parse 'HEAD^{tree}')"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
ulimit -c 0
for compiler in gcc clang; do
    flags=(-O2 -std=c11 -Wall -Wextra -Werror -pedantic)
    if [[ "$compiler" == clang ]]; then
        flags+=(-fsanitize=undefined -fno-sanitize-recover=all)
    fi
    "$compiler" "${flags[@]}" -I "$root/src/main/cpp" \
       "$root/src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c" \
       "$root/tests/actor_owned_i64_exclusive_loan.c" \
       -o "$work/actor-owned-$compiler"
    timeout --kill-after=2s 45s "$work/actor-owned-$compiler" \
       >"$work/$compiler.stdout" 2>"$work/$compiler.stderr"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_CONTENTION_PASS' "$work/$compiler.stdout"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_TYPED_FAULT_PASS' "$work/$compiler.stdout"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_FAULT_RETIREMENT_PASS' "$work/$compiler.stdout"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_QUOTA_RECOVERY_PASS' "$work/$compiler.stdout"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_POSITIVE_PASS' "$work/$compiler.stdout"
    grep -Fqx 'HOST_ONLY_EXCLUSIVE_LOAN_SUITE_PASS positives=1 abort_negatives=16' "$work/$compiler.stdout"
    test "$(grep -c '^HOST_ONLY_EXCLUSIVE_LOAN_NEGATIVE_PASS case=' "$work/$compiler.stdout")" -eq 16
    test ! -s "$work/$compiler.stderr"
    echo "HOST_ONLY_NATIVE_${compiler^^}_SHA256=$(sha256sum "$work/actor-owned-$compiler" | cut -d' ' -f1)"
    echo "HOST_ONLY_NATIVE_${compiler^^}_EXECUTED=PASS"
done
echo 'HOST_ONLY_NATIVE_EXCLUSIVE_LOAN_REGRESSION=PASS'
echo 'REAL_ATTO_LLVM_GUEST_EXECUTED=false'
echo 'REAL_DOTNET_NATIVEAOT_GUEST_EXECUTED=false'
echo 'ACTOR_LOCAL_FATAL_RECOVERY_CERTIFIED=false'
echo 'BROKER_NATIVE_CERTIFIED=false'
