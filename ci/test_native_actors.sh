#!/usr/bin/env bash
# Real native .atto actor machine-code execution (bounded early LLVM profile).
set -euo pipefail
cd "$(dirname "$0")/.."
export CARGO_NET_OFFLINE=true
test "$(git hash-object frontend/src/check.rs)" = "18c9b7e3e9696bfdd56fa5130f52a9a71a796c43"
test "$(git hash-object tests/actors/pass/180-two-types-state.atto)" = "31a7248917db4fad90069af7d2c236d7a257dfa0"
test "$(git hash-object tests/actors/pass/181-same-type-separate-state.atto)" = "08b2d59fb772886508eda38ff03de2c61e1dfd5a"
test "$(git hash-object tests/actors/pass/182-method-and-fifo.atto)" = "fd6c08535705b52972427afc940476a8ebfb6e46"
test "$(git hash-object tests/actors/pass/183-bool-mail.atto)" = "bcbf4c0a4e72abca113a0cda21c9333cd467da2a"

# The host runtime is built as C11, but the guest actor handlers are LLVM
# machine code compiled from .atto, not interpreted or simulated C handlers.
clang -std=c11 -O2 -Wall -Wextra -Werror -pthread \
  -c runtime/actor_runtime.c -o /tmp/atto_actor_runtime_check.$$.o
rm -f /tmp/atto_actor_runtime_check.$$.o
cargo test --offline
cargo build --offline --release --bin atto-llvm
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

python3 - <<'PY'
from pathlib import Path
expected = {
    "180-two-types-state.atto": (4, False),
    "181-same-type-separate-state.atto": (2, False),
    "182-method-and-fifo.atto": (4, True),
    "183-bool-mail.atto": (2, True),
}
files = list(Path("tests/actors/pass").glob("*.atto"))
assert {x.name for x in files} == set(expected), "unexpected actor pass fixture set"
for file in files:
    lines = file.read_text().splitlines()
    matches = [l.split(": ", 1)[1] for l in lines if l.startswith("// expect-stdout: ")]
    assert len(matches) == expected[file.name][0]
    assert any(l == "// stdout-order: strict" for l in lines) == expected[file.name][1]
print("PASS actor fixture evidence preflight")
PY

for case in tests/actors/pass/*.atto; do
    name="$(basename "$case" .atto)"
    target/release/atto-llvm "$case" -o "$work/$name.ll" --emit-llvm
    grep -q 'define void @atto_actor_init' "$work/$name.ll"
    grep -q 'define void @atto_actor_receive' "$work/$name.ll"
    grep -q 'declare i32 @atto_actor_spawn' "$work/$name.ll"
    grep -q 'call void @atto_actor_finish()' "$work/$name.ll"
    target/release/atto-llvm "$case" -o "$work/$name.exe"
    readelf -h "$work/$name.exe" | grep -q 'Class:[[:space:]]*ELF64'
    # Twenty independent native executions check per-actor ordering while
    # accepting different interleavings across independent actor threads.
    for iteration in $(seq 1 20); do
        set +e
        timeout 10s "$work/$name.exe" > "$work/output" 2>"$work/stderr"
        code=$?
        set -e
        if [ "$code" != 0 ]; then
            echo "FAIL $name iteration=$iteration exit=$code" >&2
            cat "$work/stderr" >&2
            exit 1
        fi
        python3 - "$case" "$work/output" <<'PY'
import pathlib,sys
source=pathlib.Path(sys.argv[1]).read_text().splitlines()
actual=pathlib.Path(sys.argv[2]).read_text().splitlines()
expected=[s.split(": ",1)[1] for s in source if s.startswith("// expect-stdout: ")]
strict="// stdout-order: strict" in source
assert (actual == expected if strict else sorted(actual)==sorted(expected)), (
    sys.argv[1], actual, expected, strict
)
PY
    done
    echo "PASS native Atto actor $name (20 executions)"
done

for case in tests/actors/fail/*.atto; do
    name="$(basename "$case" .atto)"
    if target/release/atto-llvm "$case" -o "$work/$name.exe" >"$work/negative" 2>&1; then
        echo "FAIL rejected actor source compiled: $name" >&2
        exit 1
    fi
    test ! -e "$work/$name.exe"
    if ! grep -E 'E_(PRIVATE|TYPE|TOPLEVEL|RESERVED|UNSUPPORTED|EXPECTED|PARSE|SYNTAX|UNBOUND|ISOLATION)' "$work/negative"; then
        echo "FAIL missing diagnostic in actor negative: $name" >&2
        cat "$work/negative" >&2
        exit 1
    fi
    echo "PASS fail-closed actor negative $name"
done

# Independent C ABI contention gate: the same real runtime, with C mock
# callbacks, is stressed by four producer threads and eight actor workers.
# This complements (does not replace) compiled .atto guest execution above.
clang -std=c11 -O1 -g -pthread -fsanitize=address,undefined \
  -fno-omit-frame-pointer -Wall -Wextra -Werror \
  runtime/actor_runtime.c tests/native_actor_mailbox_stress.c \
  -o "$work/mailbox_stress.exe"
for iteration in $(seq 1 5); do
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        timeout 20s "$work/mailbox_stress.exe" > "$work/mailbox_output"
    grep -q 'PASS C actor mailbox concurrency 1600 messages' "$work/mailbox_output"
done
echo "PASS 5x 1600 C ABI concurrent sends, FIFO per actor, stale refs and deterministic drain"

# Compile both LLVM IR and the native C runtime under sanitizers. Two
# independently initialized actor arenas/threads must have no UB/memory bugs.
target/release/atto-llvm tests/actors/pass/180-two-types-state.atto \
  -o "$work/sanitized.ll" --emit-llvm
clang -x ir "$work/sanitized.ll" -x c runtime/actor_runtime.c -pthread \
  -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -o "$work/sanitized.exe"
set +e
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  timeout 20s "$work/sanitized.exe" >"$work/sanitized.stdout"
sanitizer_code=$?
set -e
test "$sanitizer_code" = 0
python3 - "$work/sanitized.stdout" <<'PY'
import pathlib,sys
assert sorted(pathlib.Path(sys.argv[1]).read_text().splitlines()) == sorted(["1","3","102","105"])
PY
echo "PASS real actor LLVM native ASan/UBSan"

# Ensure we did not replace or relax the 1255-case scalar LLVM source proof.
bash ci/test_native_aot.sh

echo 'PASS LLVM_ACTOR_BOOTSTRAP: isolated arena + dedicated stack, two actor types, per-instance state, FIFO and send'
