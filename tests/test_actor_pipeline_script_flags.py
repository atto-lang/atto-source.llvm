#!/usr/bin/env python3
"""Verify native actor host-matrix dispatch without compiling private code.

This verifies the *driver*, not NativeActorPipeTransport execution. The
actual C++ compiler/sanitizer and runtime tests remain independent gates.
"""
import json
import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "scripts" / "test-actor-pipeline-routing.sh"

COMPILER = """#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['ACTOR_TEST_COMPILER_TRACE'], 'a', encoding='utf-8') as log:
    log.write(json.dumps(args) + '\\n')
output = pathlib.Path(args[args.index('-o') + 1])
output.write_text('#!/bin/sh\\nexit 0\\n', encoding='utf-8')
output.chmod(0o755)
"""


def verify(mode: int) -> None:
    with tempfile.TemporaryDirectory(prefix="ores-actor-matrix-") as temp:
        folder = Path(temp)
        bin_dir = folder / "bin"
        bin_dir.mkdir()
        for compiler in ("g++", "clang++"):
            script = bin_dir / compiler
            script.write_text(COMPILER, encoding="utf-8")
            script.chmod(0o755)

        trace = folder / "compilers.jsonl"
        trace.write_text("", encoding="utf-8")
        env = os.environ.copy()
        env["ACTOR_TEST_COMPILER_TRACE"] = str(trace)
        env["ORES_ACTOR_PIPELINE_SANITIZE"] = str(mode)
        env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
        run = subprocess.run(
            ["sh", str(DRIVER)],
            cwd=ROOT, env=env, text=True, capture_output=True, timeout=90,
            check=False,
        )
        if run.returncode:
            raise AssertionError(
                f"actor matrix mode {mode} failed: {run.stderr}\n{run.stdout}"
            )
        assert "ORES_ACTOR_PIPELINE_HOST_MATRIX_PASS" in run.stdout
        commands = [json.loads(line) for line in trace.read_text().splitlines()]
        sanitizers = [args for args in commands
                      if "-fsanitize=address,undefined" in args]
        production = [args for args in commands
                      if "tests/native_actor_pipeline_routing_test.cpp" in args
                      and not any(arg.startswith("-DORES_") for arg in args)]
        promotion = [args for args in commands
                     if any("writer-promotion-injected-" in arg for arg in args)]
        counts = (len(commands), len(sanitizers), len(production), len(promotion))
        expected = (38, 14, 6, 2) if mode else (24, 0, 4, 0)
        assert counts == expected, (
            f"wrong compiler dispatch mode={mode}: {counts} != {expected}"
        )
        print(f"ACTOR_ROUTING_SCRIPT_CONFIG_PASS mode={mode} dispatch={counts}")


if __name__ == "__main__":
    verify(0)
    verify(1)
