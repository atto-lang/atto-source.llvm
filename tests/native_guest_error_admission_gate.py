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
     "not supported"),
    ("pub fnc main(): int { raise 1; return 0; }\n",
     "not supported"),
    ("pub fnc main(): int { panic 1; return 0; }\n",
     "not supported"),
]
with tempfile.TemporaryDirectory(prefix="ores-native-error-gate-") as tmp:
    source = Path(tmp) / "main.ores"
    for text, diagnostic in cases:
        source.write_text(text, encoding="utf-8")
        out = subprocess.run(
            [args.compiler, str(source)],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=env, timeout=20)
        assert out.returncode != 0, f"unexpected admission of native source: {text}"
        if diagnostic not in out.stderr:
            # Existing unsupported exception-effect diagnostics vary by
            # exact parser path; require semantic rejection, not spelling.
            if diagnostic != "not supported":
                raise AssertionError(
                    f"expected {diagnostic!r}, got {out.stderr!r}")
print("LLVM guest Error/typed effects remain fail-closed")
