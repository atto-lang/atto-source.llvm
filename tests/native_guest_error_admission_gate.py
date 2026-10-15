#!/usr/bin/env python3
"""Ensure native LLVM rejects guest Error hierarchy until real lowering exists."""
import argparse
import os
import subprocess
import tempfile
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
args = p.parse_args()
env = dict(os.environ)
for key in list(env):
    if key.startswith("ORES_LLVM_"):
        del env[key]

cases = [
    ("define class NativeFailure extends Error as\n"
     "  int code: 1;\nend\n"
     "pub fnc main(): int { return 0; }\n",
     "native guest Error/class inheritance is not enabled"),
    ("define class Error as\n  int code: 1;\nend\n"
     "pub fnc main(): int { return 0; }\n",
     "Error is a compiler-owned guest root"),
    ("pub fnc main(): int { throw 1; return 0; }\n",
     "native exception/control effect unsupported"),
    ("pub fnc main(): int { raise 1; return 0; }\n",
     "native exception/control effect unsupported"),
    ("pub fnc main(): int { panic 1; return 0; }\n",
     "native exception/control effect unsupported"),
]
with tempfile.TemporaryDirectory(prefix="ores-native-error-gate-") as tmp:
    source = Path(tmp) / "main.ores"

    # A broken compiler invocation must never make every negative-admission
    # test appear green. First prove the exact executable emits valid IR.
    source.write_text("pub fnc main(): int { return 42; }\n", encoding="utf-8")
    healthy = subprocess.run(
        [args.compiler, str(source)], text=True,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env=env, timeout=20)
    assert healthy.returncode == 0, (
        f"native compiler positive control failed: {healthy.stderr!r}")
    assert "define " in healthy.stdout and "ret i64 42" in healthy.stdout, (
        f"native compiler did not emit expected scalar LLVM IR: "
        f"{healthy.stdout[:1200]!r}")

    for text, diagnostic in cases:
        source.write_text(text, encoding="utf-8")
        out = subprocess.run(
            [args.compiler, str(source)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, timeout=20)
        assert out.returncode != 0, f"unexpected admission of native source: {text}"
        assert diagnostic in out.stderr, (
            f"expected native parser diagnostic {diagnostic!r}, "
            f"got stderr={out.stderr!r}, stdout={out.stdout!r}")
print("LLVM guest Error/typed effects remain fail-closed after positive native IR control")
