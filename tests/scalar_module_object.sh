#!/usr/bin/env bash
# Real Atto module providers and a native host consumer. The C consumer is
# deliberately NOT a claim of source-language import or package ABI parity.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
driver="$root/scripts/compile-scalar-module-object.sh"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
cat >"$work/cart.atto" <<'ATTO'
define module cart as
  pub fnc value(): int { return 40; }
end
ATTO
cat >"$work/tax.atto" <<'ATTO'
define module tax as
  pub fnc value(): int { return 2; }
end
ATTO
cat >"$work/consumer.c" <<'C'
#include <stdint.h>
extern int64_t cart_value(void) __asm__("cart.value");
extern int64_t tax_value(void) __asm__("tax.value");
int main(void) { return (int)(cart_value() + tax_value()); }
C
clang="${CLANG:-clang}"
for opt in 0 2; do
  mkdir "$work/$opt"
  d="$work/$opt"
  export ORESLANG_LLVM_AOT_OPT_LEVEL="$opt"
  bash "$driver" "$work/cart.atto" "$d/cart.o" cart value
  bash "$driver" "$work/tax.atto" "$d/tax.o" tax value
  "$clang" "-O$opt" -c "$work/consumer.c" -o "$d/consumer.o"
  sha256sum "$d/tax.o" "$d/consumer.o" >"$d/reuse.sha"
  "$clang" "$d/cart.o" "$d/tax.o" "$d/consumer.o" -o "$d/old"
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  sed -i 's/return 40/return 41/' "$work/cart.atto"
  bash "$driver" "$work/cart.atto" "$d/new-cart.o" cart value
  cmp -s "$d/cart.o" "$d/new-cart.o" && { echo 'edit did not change object' >&2; exit 1; }
  sha256sum -c "$d/reuse.sha"
  "$clang" "$d/new-cart.o" "$d/tax.o" "$d/consumer.o" -o "$d/new"
  rc=0; "$d/new" || rc=$?; test "$rc" = 43
  rc=0; "$d/old" || rc=$?; test "$rc" = 42
  bash "$driver" "$work/cart.atto" "$d/clean-cart.o" cart value
  cmp "$d/clean-cart.o" "$d/new-cart.o"
  "$clang" "$d/clean-cart.o" "$d/tax.o" "$d/consumer.o" -o "$d/clean"
  rc=0; "$d/clean" || rc=$?; test "$rc" = 43
  sha256sum "$d/new-cart.o" >"$d/prior.sha"
  if bash "$driver" "$work/cart.atto" "$d/new-cart.o" other value; then exit 1; fi
  sha256sum -c "$d/prior.sha"
  ln -s "$d/new-cart.o" "$d/symlink.o"
  if bash "$driver" "$work/cart.atto" "$d/symlink.o" cart value; then exit 1; fi
  test -L "$d/symlink.o"; sha256sum -c "$d/prior.sha"
  printf 'define module cart as pub fnc value(int n): int { return n; } end\n' >"$work/arity.atto"
  if bash "$driver" "$work/arity.atto" "$d/wrong.o" cart value; then exit 1; fi
  test ! -e "$d/wrong.o"
  printf 'define module cart as pub fnc value(): int { return 1 + ; } end\n' >"$work/bad.atto"
  if bash "$driver" "$work/bad.atto" "$d/new-cart.o" cart value; then exit 1; fi
  sha256sum -c "$d/prior.sha"
  # Protected source aliases and special output nodes must fail without
  # altering either the original input or last admitted provider.
  sha256sum "$work/cart.atto" >"$d/source.sha"
  ln "$work/cart.atto" "$d/source-alias.o"
  if bash "$driver" "$work/cart.atto" "$d/source-alias.o" cart value; then exit 1; fi
  sha256sum -c "$d/source.sha"
  test "$d/source-alias.o" -ef "$work/cart.atto"
  mkdir "$d/directory.o"
  if bash "$driver" "$work/cart.atto" "$d/directory.o" cart value; then exit 1; fi
  test -d "$d/directory.o"
  mkfifo "$d/fifo.o"
  if bash "$driver" "$work/cart.atto" "$d/fifo.o" cart value; then exit 1; fi
  test -p "$d/fifo.o"
  sha256sum -c "$d/prior.sha"
  # Relink with no provider source present. This is still a C consumer,
  # not source-language namespace admission.
  mv "$work/cart.atto" "$work/cart.hidden"
  mv "$work/tax.atto" "$work/tax.hidden"
  "$clang" "$d/new-cart.o" "$d/tax.o" "$d/consumer.o" -o "$d/source-free"
  rc=0; "$d/source-free" || rc=$?; test "$rc" = 43
  mv "$work/cart.hidden" "$work/cart.atto"
  mv "$work/tax.hidden" "$work/tax.atto"
  sha256sum -c "$d/reuse.sha"
  sed -i 's/return 41/return 40/' "$work/cart.atto"
  printf 'REAL_ATTO_MODULE_OBJECTS|O%s|native=42/43/42/43|unchanged=2|negative=7|source_free_relink=YES|host_consumer=YES\n' "$opt"
done
