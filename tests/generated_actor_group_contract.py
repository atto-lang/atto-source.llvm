#!/usr/bin/env python3
"""Compile source-generated LLVM actor methods and execute the real ActorGroup host."""
import argparse
import pathlib
import subprocess
import sys
import tempfile

def run(args, **kwargs):
    out = subprocess.run(args, check=False, capture_output=True, text=True,
                         timeout=90, **kwargs)
    if out.returncode:
        raise AssertionError(f"failed ({out.returncode}): {' '.join(args)}\n{out.stderr}")
    return out

def main():
    parser = argparse.ArgumentParser()
    for name in ("compiler", "fixture", "cc", "cxx", "runtime", "harness", "include"):
        parser.add_argument("--" + name, required=True)
    p = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-actor-lifecycle-") as folder:
        tmp = pathlib.Path(folder)
        ir = run([p.compiler, p.fixture]).stdout
        if not ir.startswith("; ModuleID") or "ores_actor_Counter_add" not in ir:
            raise AssertionError("actor source did not compile into generated native field methods")
        if "gc.statepoint" in ir or "gcroot" in ir:
            raise AssertionError("tracing GC entered native actor field lowering")
        module = tmp / "generated.ll"
        module.write_text(ir, encoding="utf-8")
        native = tmp / "actor_heap.o"
        run([p.cc, "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", p.include, "-c", p.runtime, "-o", str(native)])
        executable = tmp / "test_actor"
        run([p.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-I", p.include, "-x", "ir", str(module),
             "-x", "c++", p.harness, "-x", "none", str(native),
             "-o", str(executable)])
        run([str(executable)])
    print("GENERATED_ACTOR_GROUP_LIFECYCLE_PASS source_ir=1 migration=1 loans=1 cancel=1 panic=1 startup_stop=1")

if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"generated actor lifecycle contract failed: {error}", file=sys.stderr)
        sys.exit(1)
