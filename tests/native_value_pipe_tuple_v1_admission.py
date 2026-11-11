#!/usr/bin/env python3
"""Compiler-backed UNSUPPORTED admission gate for canonical tuple-pipe v1.

This is intentionally a red/unsupported native feature test, NOT LLVM AOT
parity. It must be upgraded to guest execution as soon as typed tuple ABI,
ownership and lambda lowering exist. No host-only tuple interpreter.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

# Byte-exact sources fetched from conformance PR #677 and pinned by Git blobs.
# Using local copies avoids cross-repo checkout tokens and network dependency.
CASES = json.loads(r'''[{"path":"positive.atto","sha":"09b15dad59e556079bc5312cff4131134d9e38f7","source":"fnc fetch(): Tuple[int, int, int] { return tuple(4, 2, 3); }\nfnc validate(Tuple[int, int, int] value): Tuple[int, int, int] { return value; }\nfnc filter(int users, int count): Tuple[int, int, int] { return tuple(users + count, 10, 20); }\nfnc sort(Tuple[int, int, int] value): Tuple[int, int, int] { return value; }\nfnc take(int n, int k): Tuple[int, int] { return tuple(n, k); }\nfnc build(): Tuple[int, int] {\n  return fetch()\n    |> validate\n    |> |users, x, y| -> filter(users, x + y)\n    |> sort\n    |> |sorted, a, b| -> take(sorted, a + b);\n}\npub fnc main(): void {\n  stdio.println(build() |> |one, two| -> one + two);\n  return;\n}\n"},{"path":"negative/named-function-no-flatten.atto","sha":"3c216290e3efa20edf44adc4f596ced83b5b33e5","source":"fnc add(int a, int b): int { return a + b; }\npub fnc main(): void { stdio.println(tuple(1, 2) |> add); return; }\n"},{"path":"negative/scalar-not-tuple.atto","sha":"66d170340c287f6c75f47e7bfa6aaf3a7c65024e","source":"pub fnc main(): void { stdio.println(2 |> |a, b| -> a + b); return; }\n"},{"path":"negative/tuple-return-type-mismatch.atto","sha":"d93352b1304ee8292a9c51d6204e82a7bbe2cc58","source":"fnc broken(): Tuple[int, string] { return tuple(1, 2); }\npub fnc main(): void { broken(); return; }\n"},{"path":"negative/tuple-wrong-arity.atto","sha":"57f7fa9ef8086d783ce54d4fc8ce2f4ec0d2f94b","source":"pub fnc main(): void { stdio.println(tuple(1, 2) |> |a, b, c| -> a); return; }\n"}]''')

def git_blob(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode() + b"\\0".replace(b"\\0", b"\x00") + data).hexdigest()

def run_compiler(compiler: Path, source: Path, cwd: Path):
    return subprocess.run([str(compiler), str(source)], cwd=cwd, text=True,
                          capture_output=True, timeout=30, check=False)

def snapshot(root: Path):
    return {p.relative_to(root).as_posix(): p.read_bytes()
            for p in root.rglob("*") if p.is_file()}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    if not compiler.is_file():
        raise RuntimeError("compiler is not a regular file")
    with tempfile.TemporaryDirectory(prefix="atto-llvm-tuple-pipe-") as temp:
        root = Path(temp)
        scalar = root / "scalar-control.atto"
        scalar.write_text("pub fnc main(): int { return 42; }\n", encoding="utf-8")
        sanity = run_compiler(compiler, scalar, root)
        if sanity.returncode != 0 or "define i64 @__oreslang_guest_main(" not in sanity.stdout:
            raise RuntimeError(
                "no working native compiler scalar-positive control: " +
                sanity.stderr[:350] + " / " + sanity.stdout[:200])
        for case in CASES:
            raw = case["source"].encode("utf-8")
            if git_blob(raw) != case["sha"]:
                raise RuntimeError("fixture identity mismatch: " + case["path"])
            relative = Path(case["path"])
            if relative.is_absolute() or ".." in relative.parts or relative.suffix != ".atto":
                raise RuntimeError("unsafe fixture path")
            source = root / relative
            source.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(raw)
            before = snapshot(root)
            result = run_compiler(compiler, source, root)
            after = snapshot(root)
            if after != before:
                raise RuntimeError("compiler modified source or emitted artifacts for " + case["path"])
            if result.returncode <= 0:
                raise RuntimeError("unsupported tuple source unexpectedly compiled or crashed: " + case["path"])
            if result.stdout.strip():
                raise RuntimeError("rejected tuple source emitted LLVM stdout/IR: " + case["path"])
            error = result.stderr.strip().lower()
            if not error or any(
                    symptom in error for symptom in
                    ("no such file", "cannot open file", "unsupported file extension",
                     "cannot read source", "usage:")):
                raise RuntimeError("infrastructure/parser admission not proved: " + case["path"] + " " + error[:350])
    print(json.dumps({
        "schema": "atto.llvm.tuple-pipe-admission/v1",
        "status": "UNSUPPORTED",
        "scalar_compiler_control": "compiled to actual LLVM IR",
        "tuple_fixtures": len(CASES),
        "fixture_blobs": {case["path"]: case["sha"] for case in CASES},
        "positive_guest_executed": False,
        "llvm_native_aot_parity": False,
        "note": "compiler-backed rejection check; not a substitute for tuple ABI and O0/O2 native guest binaries"
    }, indent=2))

if __name__ == "__main__":
    main()
