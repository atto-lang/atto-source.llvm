#!/usr/bin/env python3
"""Self-test: actor conformance harnesses cannot silently lose asserts under -O."""
import os
from pathlib import Path
import subprocess
import sys

def run(argv, env):
    return subprocess.run(argv, capture_output=True, text=True, timeout=10, env=env)

def main():
    root = Path(__file__).resolve().parent
    env = dict(os.environ)
    env.pop("PYTHONOPTIMIZE", None)
    for script in ("source_actor_e2e.py", "wasm32_source_actor_matrix.py"):
        path = root / script
        guarded = run([sys.executable, "-O", str(path), "--help"], env)
        evidence = guarded.stdout + guarded.stderr
        if guarded.returncode == 0 or "refuses optimized Python" not in evidence:
            raise RuntimeError(
                f"{script}: -O bypassed the compiler/guest assert gate: "
                f"exit={guarded.returncode}, output={evidence!r}")
        normal = run([sys.executable, str(path), "--help"], env)
        if normal.returncode != 0 or "--compiler" not in normal.stdout:
            raise RuntimeError(
                f"{script}: regular harness CLI is broken: "
                f"exit={normal.returncode}, stderr={normal.stderr!r}")
    print("PASS actor conformance harness optimization guard: two scripts")

if __name__ == "__main__":
    main()
