#!/usr/bin/env bash
set -euo pipefail
compiler="${1:?usage: native_assert_scalar_guest_test.sh <oreslang-llvmc>}"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cat > "$work/guest.ores" <<'ORES'
import module Assert from "std/assert";
pub fnc int_equal(int a, int b): bool { return Assert.equal(a, b); }
pub fnc int_different(int a, int b): bool { return Assert.not_equal(a, b); }
pub fnc literal_equal(): bool { return Assert.equal(7, 7); }
pub fnc literal_different(): bool { return Assert.not_equal(7, 8); }
ORES

"$compiler" "$work/guest.ores" > "$work/guest.ll"
grep -E '^define (zeroext )?i1 @int_equal\\(i64' "$work/guest.ll"
grep -E '^define (zeroext )?i1 @int_different\\(i64' "$work/guest.ll"
grep -F 'icmp eq i64' "$work/guest.ll"
grep -F 'icmp ne i64' "$work/guest.ll"
if grep -E '^declare .*Assert|^declare .*assert' "$work/guest.ll"; then
  echo "unresolved Assert host symbol in generated guest IR" >&2; exit 1
fi

cat > "$work/host.cpp" <<'CPP'
#include <cstdint>
#include <limits>
#include <iostream>
extern "C" bool int_equal(std::int64_t, std::int64_t);
extern "C" bool int_different(std::int64_t, std::int64_t);
extern "C" bool literal_equal();
extern "C" bool literal_different();
int main() {
    const auto lo = std::numeric_limits<std::int64_t>::min();
    const auto hi = std::numeric_limits<std::int64_t>::max();
    if (!int_equal(0, 0) || int_equal(0, 1) ||
        !int_equal(lo, lo) || int_equal(lo, hi) ||
        int_different(0, 0) || !int_different(0, 1) ||
        !int_different(lo, hi) || int_different(hi, hi) ||
        !literal_equal() || !literal_different()) return 70;
    std::cout << "ASSERT_NATIVE_COMPILED_GUEST_PASS\n";
}
CPP
# Clang is the authoritative reference for the target C bool ABI.
# Verify the exact symbol's extension convention instead of relying on a
# linked executable that can accidentally work despite undefined high bits.
clang++ -std=c++17 -O0 -S -emit-llvm "$work/host.cpp" -o "$work/host.ll"
for symbol in int_equal int_different literal_equal literal_different; do
    host_decl="$(grep -E "^declare .*i1 @$symbol\\(" "$work/host.ll")"
    guest_def="$(grep -E "^define .*i1 @$symbol\\(" "$work/guest.ll")"
    test -n "$host_decl" && test -n "$guest_def"
    if [[ "$host_decl" == *"zeroext i1 @"* ]]; then
        [[ "$guest_def" == *"zeroext i1 @"* ]] || {
            echo "missing Clang-required zeroext on $symbol" >&2; exit 1;
        }
    else
        [[ "$guest_def" != *"zeroext i1 @"* ]] || {
            echo "unexpected target bool return zeroext on $symbol" >&2; exit 1;
        }
    fi
done
echo ASSERT_NATIVE_C_BOOL_ABI_MATCH_PASS

guest_flags=(-O2)
if test "${ASSERT_NATIVE_SANITIZE:-0}" = 1; then
  guest_flags+=(-fsanitize=address,undefined -fno-omit-frame-pointer)
fi
clang++ -std=c++17 "${guest_flags[@]}" "$work/host.cpp" -x ir "$work/guest.ll" -o "$work/run"
"$work/run" | grep -Fx ASSERT_NATIVE_COMPILED_GUEST_PASS

expect_reject() {
    local label="$1" message="$2"
    cat > "$work/reject.ores"
    local flags=()
    if test "$label" = wasm; then flags=(--target=wasm32); fi
    if "$compiler" "${flags[@]}" "$work/reject.ores" > "$work/reject.ll" 2>"$work/reject.err"; then
        echo "unexpected admission: $label" >&2; exit 1
    fi
    test ! -s "$work/reject.ll"
    cat "$work/reject.err"
    grep -F "$message" "$work/reject.err" >/dev/null
}
expect_reject throwing 'direct equal/not_equal primitive return' <<'ORES'
import module Assert from "std/assert";
pub fnc no_throw(): bool { return Assert.equal_throws(1, 2); }
ORES
expect_reject int_return 'cannot enter an int/void expression' <<'ORES'
import module Assert from "std/assert";
pub fnc main(): int { return Assert.equal(1, 2); }
ORES
expect_reject bool_to_int 'bool-to-int coercion' <<'ORES'
import module Assert from "std/assert";
pub fnc pred(): bool { return Assert.equal(1, 2); }
pub fnc main(): int { return pred(); }
ORES
expect_reject nested_borrow 'oreslang-llvmc:' <<'ORES'
import module Assert from "std/assert";
pub fnc bad(int x): bool { return Assert.equal(rt borrow x, 7); }
ORES
expect_reject nested_call 'oreslang-llvmc:' <<'ORES'
import module Assert from "std/assert";
pub fnc x(): int { return 2; }
pub fnc bad(): bool { return Assert.equal(x(), 2); }
ORES
expect_reject nested_conditional 'native Assert member requires a typed bool return' <<'ORES'
import module Assert from "std/assert";
pub fnc main(): int {
  if (1 eq 1) { Assert.equal(1, 2); } else { } fi
  return 0;
}
ORES
expect_reject nested_bool_call 'native bool-to-int coercion requires explicit lowering' <<'ORES'
import module Assert from "std/assert";
pub fnc pred(): bool { return Assert.equal(1, 1); }
pub fnc main(): int {
  if (1 eq 1) { pred(); } else { } fi
  return 0;
}
ORES
expect_reject unsupported_type 'direct equal/not_equal primitive return' <<'ORES'
import module Assert from "std/assert";
pub fnc bad(): bool { return Assert.true(1); }
ORES
expect_reject wasm 'sealed host-only import' <<'ORES'
import module Assert from "std/assert";
pub fnc pred(): bool { return Assert.equal(1, 1); }
ORES
