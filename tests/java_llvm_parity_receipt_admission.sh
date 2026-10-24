#!/usr/bin/env bash
# Synthetic executable stubs only: test receipt *rejection* by the shell gate.
# No LLVM, Java, Maven or Oreslang compiler executes in this test.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd -P)"
bash -n "$root/scripts/verify-java-llvm-source-parity.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/llvm/scripts" "$tmp/java/scripts" "$tmp/mockbin" "$tmp/build"
for kind in llvm java; do
  mkdir -p "$tmp/$kind/tests/fixtures/native-class-string"
  printf 'pub fnc main():int{return 0;}\n' > "$tmp/$kind/tests/fixtures/native-class-string/main.ores"
  printf 'define pub class Foo as end\n' > "$tmp/$kind/tests/fixtures/native-class-string/main-1.ores"
done
mkdir -p "$tmp/llvm/src/main/cpp/oreslang/runtime"
printf '/* not compiled */\n' > "$tmp/llvm/src/main/cpp/oreslang/runtime/NativeStringRegion.c"
cp "$root/scripts/verify-java-llvm-source-parity.sh" "$tmp/llvm/scripts/verify-java-llvm-source-parity.sh"
printf '#!/bin/sh\nexit 0\n' > "$tmp/java/scripts/run-source-parity.sh"
chmod +x "$tmp/java/scripts/run-source-parity.sh"
for kind in llvm java; do
  git -C "$tmp/$kind" init -q
  git -C "$tmp/$kind" add .
  git -C "$tmp/$kind" -c user.name=SyntheticAdmission -c user.email=synthetic@invalid.example commit -qm initial
done

# Mocked build operations must never access Oreslang source; they only make
# synthetic binaries so the release receipt validation path can be reached.
cat > "$tmp/mockbin/cmake" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
if [[ "$1" == "--build" ]]; then
  install -m 755 "$MOCK_HARNESS" "$2/oreslang-native-class-string-parity"
  printf '#!/bin/sh\nexit 0\n' > "$2/oreslang-llvmc"
  chmod +x "$2/oreslang-llvmc"
fi
MOCK
cat > "$tmp/mockbin/mvn" <<'MOCK'
#!/bin/sh
exit 0
MOCK
cat > "$tmp/mock-harness" <<'MOCK'
#!/usr/bin/env bash
set -euo pipefail
while (( $# )); do
  case "$1" in
    --report) shift; cp "$MOCK_RECEIPT" "$1";;
  esac
  shift
done
exit 0
MOCK
chmod +x "$tmp/mockbin/cmake" "$tmp/mockbin/mvn" "$tmp/mock-harness"
export MOCK_HARNESS="$tmp/mock-harness" MOCK_RECEIPT="$tmp/fake-receipt.json"
export PATH="$tmp/mockbin:$PATH"
llvm_sha="$(git -C "$tmp/llvm" rev-parse HEAD)"
java_sha="$(git -C "$tmp/java" rev-parse HEAD)"

names='["exact-user-program","string-content-equality","string-equality-embedded-nul-and-unicode","http-string-routing-primitives","source-effect-order","parameters-and-return","lazy-conditional","escaped-unicode","empty-and-embedded-nul"]'
# Expected hexadecimal stdout is independent of the mocked harness.
expected='{"exact-user-program":"68656c6c6f20776f726c640a686969696920326869696969","string-content-equality":"657175616c0a646966666572656e740a656d7074790a","string-equality-embedded-nul-and-unicode":"6e756c0a6c656e6774680a756e69636f64650a","http-string-routing-primitives":"6865616c74680a6d697373696e670a6d6574686f640a","source-effect-order":"6265666f72653b6d6574686f643b61667465723b6d6574686f643b6161","parameters-and-return":"6c65667472696768740a","lazy-conditional":"796573","escaped-unicode":"c3a9f09f98800a09225c","empty-and-embedded-nul":"610062"}'
make_receipt() {
  jq -n --arg llvm "$llvm_sha" --arg java "$java_sha" --argjson names "$names" --argjson expected "$expected" '{
    schema:"oreslang.java-llvm.source-parity.v1",status:"passed",
    llvm_revision:$llvm,java_revision:$java,positive_programs:9,
    native_executions:45,java_executions:18,llvm_only_negative_rejections:16,
    sanitized:false,cases:[$names[] | {
      name:.,native_stdout_hex:$expected[.],java_stdout_hex:$expected[.],
      native_exit:0,java_exit:0,native_runs:5,java_runs:2
    }]
  }' > "$MOCK_RECEIPT"
}
expect_bad_receipt() {
  local description="$1" observed=0
  bash "$tmp/llvm/scripts/verify-java-llvm-source-parity.sh" \
    "$tmp/build" "$tmp/java" "$tmp/java/scripts/run-source-parity.sh" clang \
    > "$tmp/run.stdout" 2> "$tmp/run.stderr" || observed="$?"
  if [[ "$observed" != 65 ]] || ! grep -q 'invalid strict parity receipt' "$tmp/run.stderr"; then
    echo "FAIL $description: expected invalid receipt exit 65, got $observed" >&2
    cat "$tmp/run.stderr" >&2
    exit 1
  fi
  if [[ -e "$tmp/build/java-llvm-source-parity.json" ]]; then
    echo "FAIL $description: published a false positive receipt" >&2
    exit 1
  fi
  echo "PASS denied $description"
}

printf 'PASS but not JSON\n' > "$MOCK_RECEIPT"
expect_bad_receipt 'non-JSON success text'
make_receipt
cp "$MOCK_RECEIPT" "$tmp/duplicate.json"
cat "$tmp/duplicate.json" >> "$MOCK_RECEIPT"
expect_bad_receipt 'multiple concatenated JSON receipts'
make_receipt
jq '.java_revision="0000000000000000000000000000000000000000"' "$MOCK_RECEIPT" > "$tmp/new.json"
mv "$tmp/new.json" "$MOCK_RECEIPT"
expect_bad_receipt 'forged Java source SHA'
make_receipt
jq '.cases=.cases[:-1]' "$MOCK_RECEIPT" > "$tmp/new.json"
mv "$tmp/new.json" "$MOCK_RECEIPT"
expect_bad_receipt 'incomplete test corpus'
make_receipt
jq '.cases[0].java_stdout_hex="ff"' "$MOCK_RECEIPT" > "$tmp/new.json"
mv "$tmp/new.json" "$MOCK_RECEIPT"
expect_bad_receipt 'mismatched binary guest output'
make_receipt
jq '.cases[0].native_stdout_hex="0"' "$MOCK_RECEIPT" > "$tmp/new.json"
mv "$tmp/new.json" "$MOCK_RECEIPT"
expect_bad_receipt 'malformed hex bytes'
make_receipt
jq '.cases |= map(.native_stdout_hex="6162" | .java_stdout_hex="6162")' "$MOCK_RECEIPT" > "$tmp/new.json"
mv "$tmp/new.json" "$MOCK_RECEIPT"
expect_bad_receipt 'identically incorrect guest output'

# A fully correct synthetic receipt must be accepted by schema validation;
# this does NOT mean the synthetic compilers executed any Oreslang source.
make_receipt
bash "$tmp/llvm/scripts/verify-java-llvm-source-parity.sh" \
  "$tmp/build" "$tmp/java" "$tmp/java/scripts/run-source-parity.sh" clang \
  > "$tmp/run.stdout" 2> "$tmp/run.stderr"
if ! jq -e '.status == "passed" and (.cases | length) == 9' \
  "$tmp/build/java-llvm-source-parity.json" >/dev/null; then
  echo 'FAIL complete synthetic receipt was not admitted' >&2
  exit 1
fi
echo 'PASS: seven forged receipts rejected; valid synthetic schema admitted (no real compiler execution)'
