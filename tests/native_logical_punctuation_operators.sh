#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
ulimit -c 0 || true
if [[ $# -ne 2 ]]; then
  echo "usage: $0 <oreslang-llvmc> <clang>" >&2
  exit 2
fi
compiler="$1"
clang="$2"
fixtures="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fixtures/logical-punctuation-v1"
tmp="$(mktemp -d "${TMPDIR:-/tmp}/atto-punct-ops.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT
for executable in "$compiler" "$clang"; do
  if [[ ! -x "$executable" ]]; then
    echo "compiler or clang not executable: $executable" >&2
    exit 2
  fi
done
# Portable wall-clock limits: GNU timeout, Homebrew gtimeout, or a perl alarm
# fallback (macOS has no timeout(1)). Exit codes mirror GNU timeout: 124 on
# expiry, 128+signal when the child dies by a signal.
if ! command -v timeout >/dev/null; then
  if command -v gtimeout >/dev/null; then
    timeout() { gtimeout "$@"; }
  else
    timeout() {
      local secs="${1%s}"; shift
      perl -e 'my $t = shift; my $pid = fork(); die "fork: $!" unless defined $pid;
               if (!$pid) { exec @ARGV; exit 127 }
               $SIG{ALRM} = sub { kill "KILL", $pid; waitpid($pid, 0); exit 124 };
               alarm $t; waitpid($pid, 0); my $s = $?;
               exit(($s & 127) ? 128 + ($s & 127) : ($s >> 8))' "$secs" "$@"
    }
  fi
fi
ir="$tmp/positive.ll"
if ! timeout 40s "$compiler" --target=host "$fixtures/scalar-parity.atto" > "$ir"; then
  echo "native compiler rejected positive .atto source" >&2
  exit 1
fi
if [[ ! -s "$ir" ]]; then
  echo "native compiler emitted empty IR" >&2
  exit 1
fi
for opt in 0 2; do
  binary="$tmp/native-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/native-O$opt.stdout" 2>"$tmp/native-O$opt.stderr" || status=$?
  if [[ -s "$tmp/native-O$opt.stdout" ]]; then
    echo "native guest emitted unexpected stdout at O$opt" >&2
    exit 1
  fi
  if [[ "$status" -ne 42 ]]; then
    echo "native guest returned $status at O$opt, expected 42" >&2
    exit 1
  fi
done
# Lowercase operator words remain ordinary user identifiers (not keywords).
identifier_ir="$tmp/keyword-identifiers.ll"
timeout 40s "$compiler" --target=host "$fixtures/keyword-identifiers.atto" > "$identifier_ir"
if [[ ! -s "$identifier_ir" ]]; then
  echo "compiler emitted empty IR for keyword identifiers" >&2
  exit 1
fi
for opt in 0 2; do
  binary="$tmp/identifiers-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$identifier_ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/identifiers-O$opt.stdout" 2>"$tmp/identifiers-O$opt.stderr" || status=$?
  if [[ -s "$tmp/identifiers-O$opt.stdout" ]]; then
    echo "identifier guest emitted unexpected stdout at O$opt" >&2
    exit 1
  fi
  if [[ "$status" -ne 42 ]]; then
    echo "identifier fixture returned $status at O$opt, expected 42" >&2
    exit 1
  fi
done
# Java LogicalPunctuationOperatorTest.tokensAreAdjacentAndWordsRemainOrdinaryIdentifiers:
# `val and: bool` locals, adjacent `a^^b`, and operator words inside a comment.
annotated_ir="$tmp/val-annotated-identifiers.ll"
timeout 40s "$compiler" --target=host "$fixtures/val-annotated-identifiers.atto" >"$annotated_ir"
test -s "$annotated_ir" || { echo "missing annotated identifier IR" >&2; exit 1; }
for opt in 0 2; do
  binary="$tmp/annotated-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$annotated_ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/annotated-O$opt.stdout" 2>"$tmp/annotated-O$opt.stderr" || status=$?
  if [[ "$status" -ne 42 || -s "$tmp/annotated-O$opt.stdout" ]]; then
    echo "annotated identifier guest failed at O$opt: status=$status" >&2
    exit 1
  fi
done
# A real runtime-trap RHS must be skipped by false && and true ||.
# These executable tests go beyond the pure truth table and verify CFG behavior.
short_ir="$tmp/short-circuit-runtime.ll"
timeout 40s "$compiler" --target=host "$fixtures/short-circuit-runtime.atto" >"$short_ir"
test -s "$short_ir" || { echo "missing short-circuit IR" >&2; exit 1; }
for opt in 0 2; do
  binary="$tmp/short-circuit-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$short_ir" -o "$binary"
  status=0
  timeout 20s "$binary" >"$tmp/short-circuit-O$opt.stdout" 2>"$tmp/short-circuit-O$opt.stderr" || status=$?
  if [[ "$status" -ne 42 || -s "$tmp/short-circuit-O$opt.stdout" ]]; then
    echo "short-circuit runtime guest failed at O$opt: status=$status" >&2
    cat "$tmp/short-circuit-O$opt.stderr" >&2
    exit 1
  fi
done
echo "ATTO_LOGICAL_SHORT_CIRCUIT_RUNTIME_O0_O2_PASS"

# Observable side-effects provide a stronger ordering proof than pure
# boolean truth tables: the two skipped rhs() calls must emit nothing,
# followed by exactly one left and one right emission from ^^.
order_ir="$tmp/xor-order-once.ll"
# Native stdio (including int output) uses the compiler-owned string region.
# Its reactor admits pipes and sockets, not a redirected regular output file.
root="$(cd "$fixtures/../../.." && pwd -P)"
string_runtime="$root/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
test -f "$string_runtime" || { echo "missing pinned native String runtime" >&2; exit 2; }
timeout 40s "$compiler" --target=host "$fixtures/xor-order-once.atto" >"$order_ir"
test -s "$order_ir" || { echo "missing XOR order IR" >&2; exit 1; }
printf '1\n2\n' >"$tmp/xor-order.expected"
for opt in 0 2; do
  binary="$tmp/xor-order-O$opt"
  timeout 40s "$clang" "-O$opt" -x ir "$order_ir" -x c -std=c11 \
    -I "$root/src/main/cpp" "$string_runtime" -pthread -o "$binary"
  status=0
  timeout 20s "$binary" 2>"$tmp/xor-order-O$opt.stderr" |
    cat >"$tmp/xor-order-O$opt.stdout" || status=$?
  if [[ "$status" -ne 42 ]] || ! cmp -s "$tmp/xor-order.expected" "$tmp/xor-order-O$opt.stdout"; then
    echo "XOR operand exactly-once left-to-right proof failed at O$opt: status=$status" >&2
    cat "$tmp/xor-order-O$opt.stdout" >&2
    cat "$tmp/xor-order-O$opt.stderr" >&2
    exit 1
  fi
done
echo "ATTO_LOGICAL_XOR_LEFT_RIGHT_EXACT_ONCE_O0_O2_PASS"

# XOR always visits RHS, whereas AND/OR visit RHS only when needed.
# llvm.trap is emitted by checked Integer.rem with zero divisor.
for name in xor-eager-trap and-reached-rhs-trap or-reached-rhs-trap; do
  ir="$tmp/$name.ll"
  timeout 40s "$compiler" --target=host "$fixtures/$name.atto" >"$ir"
  test -s "$ir" || { echo "missing runtime trap IR for $name" >&2; exit 1; }
  grep -q 'llvm.trap' "$ir" || { echo "expected checked runtime trap in $name" >&2; exit 1; }
  for opt in 0 2; do
    binary="$tmp/$name-O$opt"
    timeout 40s "$clang" "-O$opt" -x ir "$ir" -o "$binary"
    status=0
    timeout 20s "$binary" >"$tmp/$name-O$opt.stdout" 2>"$tmp/$name-O$opt.stderr" || status=$?
    if (( status <= 128 || status == 124 || status == 125 || status == 126 || status == 127 )); then
      echo "expected native guest trap for $name at O$opt; got status=$status" >&2
      exit 1
    fi
    if [[ -s "$tmp/$name-O$opt.stdout" ]]; then
      echo "unexpected stdout from trapping $name at O$opt" >&2
      exit 1
    fi
  done
  echo "ATTO_LOGICAL_TRAP_RUNTIME_O0_O2_PASS name=$name"
done

for name in reject-int-and reject-int-or reject-int-xor reject-int-and-dead-rhs reject-int-or-dead-rhs reject-int-xor-left reject-single-caret reject-spaced-caret reject-bare-pipe reject-bare-ampersand; do
  out="$tmp/$name.ll"
  diagnostic="$tmp/$name.stderr"
  status=0
  timeout 40s "$compiler" --target=host "$fixtures/$name.atto" >"$out" 2>"$diagnostic" || status=$?
  if [[ "$status" -eq 0 ]]; then
    echo "native compiler accepted invalid boolean punctuation fixture $name" >&2
    exit 1
  fi
  if [[ "$status" -eq 124 || "$status" -eq 125 || "$status" -eq 126 || "$status" -eq 127 ]]; then
    echo "compiler timed out or was unavailable (not a valid compile-negative): $name, status=$status" >&2
    exit 1
  fi
  if [[ -s "$out" ]]; then
    echo "compiler emitted executable IR after rejection: $name" >&2
    exit 1
  fi
  if [[ ! -s "$diagnostic" ]]; then
    echo "compiler provided no diagnostic for $name" >&2
    exit 1
  fi
done
echo "logical punctuation: native O0/O2 + short-circuit/trap execution + compile negatives passed"
