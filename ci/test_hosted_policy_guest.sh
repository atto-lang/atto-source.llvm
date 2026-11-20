#!/usr/bin/env bash
# Independent real native LLVM guest execution of canonical portable .atto
# HTTP/FS pure-policy helpers. NOT HTTP/FS transport, package linking or async.
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true

test "$(git hash-object stdlib/std/http_response_policy.atto)" = 22757dafe89bc9601d04c8726a09ddf676206c7e
test "$(git hash-object stdlib/std/fs_text_policy.atto)" = 0cd53216e91ef8de1a764ba5b80ce85c5796c1f2
test "$(git hash-object fixtures/hosted-policy-main.atto)" = 0153665aac3e7cc51c8ac3412a11c0ab51cbb6be
test "$(git hash-object fixtures/hosted-policy-vectors-main.atto)" = fab89196d07080d9ec2a9902220b82a5f13e7bff

cargo build --offline --release --bin atto-llvm
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Compile TWO genuinely independent guest entries: manually curated
# boundary tests and all 56 canonical vector records.
for fixture in hosted-policy-main hosted-policy-vectors-main; do
  cat stdlib/std/http_response_policy.atto \
      stdlib/std/fs_text_policy.atto \
      "fixtures/$fixture.atto" > "$work/$fixture.atto"
  target/release/atto-llvm "$work/$fixture.atto" -o "$work/$fixture.ll" --emit-llvm
  grep -q 'define i32 @main' "$work/$fixture.ll"
  clang -x ir -O2 -fno-lto -o "$work/$fixture.exe" "$work/$fixture.ll"
  set +e
  timeout 15s "$work/$fixture.exe"
  status=$?
  set -e
  if [ "$status" != 42 ]; then
    echo "LLVM_HOSTED_POLICY_GUEST_FAILED fixture=$fixture rc=$status" >&2
    exit 1
  fi
  # Check the same compiled guest under sanitizers. Scalar-only:
  # does NOT establish tenant or actor heap/runtime isolation.
  clang -x ir -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -o "$work/$fixture-sanitized.exe" "$work/$fixture.ll"
  set +e
  ASAN_OPTIONS=detect_leaks=1 timeout 30s "$work/$fixture-sanitized.exe"
  sanitized=$?
  set -e
  if [ "$sanitized" != 42 ]; then
    echo "LLVM_HOSTED_POLICY_SANITIZER_FAILED fixture=$fixture rc=$sanitized" >&2
    exit 1
  fi
  echo "PASS LLVM_HOSTED_POLICY_NATIVE_GUEST fixture=$fixture exit=42 asan=1 ubsan=1"
done

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
echo "PASS LLVM_HOSTED_POLICY_NATIVE_PARITY fixtures=2 vector_cases=56 unsupported_io=no_artifact"
