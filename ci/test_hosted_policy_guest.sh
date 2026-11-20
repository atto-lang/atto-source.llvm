#!/usr/bin/env bash
# Independent real native LLVM guest execution of canonical portable .atto
# HTTP/FS pure-policy helpers. NOT HTTP/FS transport, package linking or async.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true

test "$(git hash-object stdlib/std/http_response_policy.atto)" = 22757dafe89bc9601d04c8726a09ddf676206c7e
test "$(git hash-object stdlib/std/fs_text_policy.atto)" = 0cd53216e91ef8de1a764ba5b80ce85c5796c1f2
test "$(git hash-object fixtures/hosted-policy-main.atto)" = 0153665aac3e7cc51c8ac3412a11c0ab51cbb6be

cargo build --offline --release --bin atto-llvm
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cat stdlib/std/http_response_policy.atto \
    stdlib/std/fs_text_policy.atto \
    fixtures/hosted-policy-main.atto > "$work/hosted-policy.atto"
target/release/atto-llvm "$work/hosted-policy.atto" -o "$work/hosted-policy.ll" --emit-llvm
grep -q 'define i32 @main' "$work/hosted-policy.ll"
clang -x ir -O2 -fno-lto -o "$work/hosted-policy.exe" "$work/hosted-policy.ll"
set +e
timeout 15s "$work/hosted-policy.exe"
status=$?
set -e
if [ "$status" != 42 ]; then
  echo "LLVM_HOSTED_POLICY_GUEST_FAILED rc=$status" >&2
  exit 1
fi
# Check the same compiled guest under sanitizers. This is a scalar-only
# regression and does NOT establish actor, heap or tenant runtime isolation.
clang -x ir -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o "$work/hosted-policy-sanitized.exe" "$work/hosted-policy.ll"
set +e
ASAN_OPTIONS=detect_leaks=1 timeout 30s "$work/hosted-policy-sanitized.exe"
sanitized=$?
set -e
if [ "$sanitized" != 42 ]; then
  echo "LLVM_HOSTED_POLICY_SANITIZER_FAILED rc=$sanitized" >&2
  exit 1
fi

# Do not turn unsupported real I/O into a fake successful scalar guest.
cat > "$work/hosted-io-unsupported.atto" <<'ATTO'
pub susp fnc main(): int {
  val response = await http.get("http://127.0.0.1:8080/data");
  val body = await response.text();
  await fs.write_text("./response.txt", body);
  val saved = await fs.read_text("./response.txt");
  return saved.length;
}
ATTO
if target/release/atto-llvm "$work/hosted-io-unsupported.atto" \
  -o "$work/hosted-io-unsupported.ll" --emit-llvm >"$work/negative.log" 2>&1; then
  echo "LLVM_UNIMPLEMENTED_HOSTED_IO_FALSE_PASS" >&2
  exit 1
fi
test ! -e "$work/hosted-io-unsupported.ll"
echo "PASS LLVM_HOSTED_POLICY_NATIVE_GUEST exit=42 asan=1 ubsan=1 unsupported_io=no_artifact"
