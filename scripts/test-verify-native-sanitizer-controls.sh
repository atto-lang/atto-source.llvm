#!/usr/bin/env bash
# Negative mutation tests for ASan/UBSan/TSan detector gates (host-only).
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
control="$root/scripts/verify-native-sanitizer-controls.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

expect_inconclusive() {
  local label="$1"
  shift
  local result
  set +e
  "$@" >"$tmp/output" 2>"$tmp/diagnostics"
  result=$?
  set -e
  if [[ "$result" -ne 2 ]]; then
    printf 'FAIL_SANITIZER_NEGATIVE_CONTROL label=%s exit=%s (expected 2)\n' \
      "$label" "$result" >&2
    cat "$tmp/output" "$tmp/diagnostics" >&2
    exit 1
  fi
  if ! grep -Fq 'INCONCLUSIVE' "$tmp/diagnostics" &&
     ! grep -Fq 'Unknown sanitizer control:' "$tmp/diagnostics"; then
    printf 'FAIL_SANITIZER_NEGATIVE_CONTROL label=%s missing fail-closed marker\n' \
      "$label" >&2
    cat "$tmp/diagnostics" >&2
    exit 1
  fi
  printf 'PASS_SANITIZER_NEGATIVE_CONTROL label=%s\n' "$label"
}

expect_inconclusive unsupported-kind bash "$control" not-a-sanitizer
expect_inconclusive missing-compiler env SANITIZER_CC="$tmp/missing-compiler" \
  bash "$control" address

# Intentionally strip sanitizer instrumentation during both compile and link.
cat >"$tmp/unsanitized-compiler" <<'WRAPPER'
#!/usr/bin/env bash
set -euo pipefail
filtered=()
for arg in "$@"; do
  case "$arg" in
    -fsanitize=*|-fno-sanitize-recover=all) ;;
    *) filtered+=("$arg") ;;
  esac
done
exec "$REAL_CC" "${filtered[@]}"
WRAPPER
chmod 700 "$tmp/unsanitized-compiler"
for kind in address undefined thread; do
  compiler=clang
  if [[ "$kind" == thread ]]; then compiler=gcc; fi
  command -v "$compiler" >/dev/null || {
    echo "missing required local control compiler: $compiler" >&2
    exit 2
  }
  expect_inconclusive "stripped-$kind" env REAL_CC="$(command -v "$compiler")" \
    SANITIZER_CC="$tmp/unsanitized-compiler" bash "$control" "$kind"
done

printf 'SANITIZER_NEGATIVE_CONTROLS_PASS count=5\n'
