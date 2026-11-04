#!/usr/bin/env python3
"""Real .atto compiler -> LLVM IR -> Clang -> native executable stack M1 slice.

An early checked frame failure returns native status 75, WITHOUT OS stack
overflow or process abort. This is still scalar-only; no actor/supervisor parity.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

# 75 = bounded guest frame exhaustion; 42 = ordinary process return.
CASES = {
    "shallow": ("pub fnc main(): int { return 42; }\n", 42),
    "self_non_tail": (
        "fnc descend(int n): int { return 1 + descend(n - 1); }\n"
        "pub fnc main(): int { return descend(100000); }\n", 75),
    "mutual_non_tail": (
        "fnc left(int n): int { return 1 + right(n - 1); }\n"
        "fnc right(int n): int { return 1 + left(n - 1); }\n"
        "pub fnc main(): int { return left(100000); }\n", 75),
    "tail_not_yet_optimized": (
        "fnc down(int n): int { return down(n - 1); }\n"
        "pub fnc main(): int { return down(100000); }\n", 75),
    "overflow_on_unwind_blocked": (
        "fnc descend(int n): int { return 9223372036854775807 + descend(n - 1); }\n"
        "pub fnc main(): int { return descend(100000); }\n", 75),
}

# Counts include OS guest main: 63 helpers + main = 64 legal frames.
def chain(length):
    functions = []
    for i in range(length):
        target = "42" if i == length - 1 else f"f{i+1}()"
        functions.append(f"fnc f{i}(): int {{ return {target}; }}")
    functions.append("pub fnc main(): int { return f0(); }")
    return "\n".join(functions) + "\n"

CASES.update({
    "acyclic_depth_64": (chain(63), 42),
    "acyclic_depth_65": (chain(64), None),
    "conditional_hidden_self_cycle": (
        "fnc loop(int n): int { return n == 0 ? 0 : loop(n - 1); }\n"
        "pub fnc main(): int { return 42; }\n", 42),
    "unreachable_self_cycle": (
        "fnc unused(int n): int { return unused(n - 1); }\n"
        "pub fnc main(): int { return 42; }\n", 42),
    "arity_overloads_acyclic": (
        "fnc f(): int { return 41; }\n"
        "fnc f(int value): int { return value + 1; }\n"
        "pub fnc main(): int { return f(f()); }\n", 42),
    "exported_recursive_helper_denied": (
        "pub fnc descend(int n): int { return descend(n - 1); }\n"
        "pub fnc main(): int { return descend(100000); }\n", None),
})

def run(argv, timeout=20):
    return subprocess.run(argv, capture_output=True, stdin=subprocess.DEVNULL,
                          timeout=timeout, check=False)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    args = ap.parse_args()
    for tool in (args.compiler, args.clang):
        if not tool.is_file():
            raise RuntimeError("required compiler/Clang missing; not an optional skip")
    binary = str(args.compiler.resolve())
    clang = str(args.clang.resolve())
    with tempfile.TemporaryDirectory(prefix="atto-native-stack-m1-") as work:
        root = Path(work)
        compiler_digest = hashlib.sha256(args.compiler.read_bytes()).hexdigest()
        observations = 0
        for label, (source, expected_exit) in CASES.items():
            src = root / (label + ".atto")
            src.write_text(source, encoding="utf-8")
            compiled = run([binary, str(src)])
            if expected_exit is None:
                if compiled.returncode == 0 or compiled.stdout:
                    raise RuntimeError(label + ": unsafe source emitted native IR")
                if b"native guest" not in compiled.stderr and b"native bounded" not in compiled.stderr:
                    raise RuntimeError(label + ": expected stack admission diagnostic missing")
                print("PASS native fail-closed admission", label)
                observations += 1
                continue

            if compiled.returncode or b"define i32 @main(" not in compiled.stdout:
                raise RuntimeError(label + ": LLVM compiler did not emit OS main: "
                                   + repr(compiled.stderr[:400]))
            if label != "shallow" and label not in ("acyclic_depth_64", "arity_overloads_acyclic"):
                if b"__atto_guest_depth" not in compiled.stdout or b"__atto_guest_exhausted" not in compiled.stdout:
                    raise RuntimeError(label + ": generated native checked frame ABI missing")
            ir = root / (label + ".ll")
            ir.write_bytes(compiled.stdout)
            exe = root / label
            linked = run([clang, "-O0", "-x", "ir", str(ir), "-o", str(exe)])
            if linked.returncode or not exe.is_file():
                raise RuntimeError(label + ": Clang LLVM IR linking failed: " + repr(linked.stderr[:400]))
            observed = run([str(exe.resolve())])
            if observed.returncode != expected_exit or observed.stdout or observed.stderr:
                raise RuntimeError(label + ": unexpected native status/stdout/stderr: "
                                   + repr((observed.returncode, observed.stdout, observed.stderr)))
            print("PASS real AttoLang -> LLVM -> native", label,
                  "exit", observed.returncode,
                  "ir_sha256", hashlib.sha256(compiled.stdout).hexdigest(),
                  "executable_sha256", hashlib.sha256(exe.read_bytes()).hexdigest())
            observations += 1
        if observations != len(CASES):
            raise RuntimeError("missing executed case count")
        print("AttoLang stack compiled_native_cases", observations,
              "compiler_sha256", compiler_digest)
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("AttoLang LLVM checked-frame parity NOT_VERIFIED:", error, file=sys.stderr)
        sys.exit(1)
