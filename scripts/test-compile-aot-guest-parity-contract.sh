#!/usr/bin/env bash
# HOST-ONLY synthetic negative control: never count this as AttoLang guest parity.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"
command -v clang >/dev/null || { echo 'Clang required for host-only negative control' >&2; exit 69; }
tmp="$(mktemp -d)"
trap 'rm -rf -- "$tmp"' EXIT
mkdir -p "$tmp/repo/scripts" "$tmp/repo/contracts/atto-parity-v1" "$tmp/repo/tests/fixtures" "$tmp/bin"
cp -- "$root/scripts/compile-aot.sh" "$root/scripts/test-compile-aot-guest-parity.sh" "$tmp/repo/scripts/"
printf '%s\n' 'pub fnc main(): int { return 40 + 2; }' > "$tmp/repo/contracts/atto-parity-v1/add.atto"
printf '%s\n' 'pub fnc main(): int { return 42; }' > "$tmp/repo/tests/fixtures/numerics-atto-main.atto"
cat > "$tmp/bin/oreslang-llvmc" <<'COMPILER'
#!/usr/bin/env bash
set -euo pipefail
[[ $# == 2 && "$1" == --target=host && -f "$2" ]] || exit 64
if grep -Eq '\+ *;' "$2"; then
  echo 'synthetic compiler rejects malformed source' >&2
  exit 65
fi
# Explicit only-on-O2 difference; missing level behaves as O0. This catches
# probing the frontend before setting the chosen optimization level.
if [[ "${ATTO_CONTRACT_DIVERGE:-0}" == 1 && "$2" == */add.atto && "${ORESLANG_LLVM_AOT_OPT_LEVEL:-0}" == 2 ]]; then
  printf '%s\n' '; O2-only frontend semantic mutation'
fi
cat <<'IR'
define i32 @main() {
entry:
  ret i32 42
}
IR
COMPILER
chmod 700 "$tmp/bin/oreslang-llvmc"
(
  cd "$tmp/repo"
  git init -q
  git add scripts contracts tests
  git -c user.name='Atto contract test' -c user.email='atto-test@invalid.example' \
    -c core.hooksPath=/dev/null commit -qm 'synthetic test fixtures'
)
export EXPECTED_HEAD="$(git -C "$tmp/repo" rev-parse HEAD)"
export ORESLANG_LLVMC="$tmp/bin/oreslang-llvmc"
export CLANG="$(command -v clang)"
unset ORESLANG_LLVM_AOT_OPT_LEVEL
if ! bash "$tmp/repo/scripts/test-compile-aot-guest-parity.sh" > "$tmp/positive.stdout" 2> "$tmp/positive.stderr"; then
  cat "$tmp/positive.stderr" >&2
  echo 'synthetic baseline unexpectedly failed' >&2
  exit 1
fi
grep -Fq 'ATTO_LLVM_GUEST_MATRIX_PASS|' "$tmp/positive.stdout" || { echo 'missing synthetic baseline marker' >&2; exit 1; }
if ATTO_CONTRACT_DIVERGE=1 bash "$tmp/repo/scripts/test-compile-aot-guest-parity.sh" > "$tmp/negative.stdout" 2> "$tmp/negative.stderr"; then
  echo 'frontend IR O0/O2 divergence was not rejected' >&2
  exit 1
fi
grep -Fq 'frontend IR not deterministic across Clang options' "$tmp/negative.stderr" || {
  cat "$tmp/negative.stderr" >&2
  echo 'IR divergence was rejected for the wrong reason' >&2
  exit 1
}
printf '%s\n' 'HOST_ONLY_PARITY_HARNESS_CONTRACT_PASS|synthetic_baseline=1|o0_o2_ir_divergence_rejected=1|real_compiler=NOT_EXECUTED|atto_guest=NOT_EXECUTED'
