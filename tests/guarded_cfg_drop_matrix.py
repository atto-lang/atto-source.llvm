#!/usr/bin/env python3
"""Regression: each CFG exit runs one native drop/close and all class lifetimes."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(cmd, *, timeout=30):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--native-runtime", required=True)
    args = p.parse_args()
    runtime = Path(args.native_runtime)
    assert runtime.exists(), runtime
    with tempfile.TemporaryDirectory(prefix="ores-cfg-drops-") as tmp:
        d = Path(tmp)
        count = 0
        for guards in (1, 2, 4, 8):
            for chosen in (0, guards):  # first taken vs final fallthrough
                tests = []
                for i in range(guards):
                    target = 7 if i == chosen else 100 + i
                    tests.append(
                        f"  if (p.a eq {target}) {{ return p.b; }} fi")
                text = (
                    "define class Pair as\n"
                    "  pub let int a = 7;\n"
                    "  pub let int b = 42;\nend\n"
                    "pub fnc main(): int {\n"
                    "  const owned_value: owned int = 1;\n"
                    "  let Pair p = new Pair();\n"
                    + "\n".join(tests) + "\n"
                    "  return owned_value + 41;\n}\n")
                source = d / f"guard-{guards}-choice-{chosen}.ores"
                source.write_text(text)
                compiled = run([args.compiler, "--experimental-owned-int", str(source)])
                assert compiled.returncode == 0 and not compiled.stderr, (
                    source.name, compiled.stderr)
                ir = compiled.stdout
                assert ir.count("exit.cleanup:") == 1, (
                    source.name, "missing one shared exit cleanup block")
                assert ir.count("call void @oreslang_owned_i64_region_drop(") == 1, (
                    source.name, "missing or duplicate ownership drop")
                assert ir.count("call void @oreslang_owned_i64_region_close(") == 1, (
                    source.name, "missing or duplicate region closure")
                assert ir.count("call void @llvm.lifetime.end") >= 1, (
                    source.name, "missing lexical class cleanup")
                assert "gc.statepoint" not in ir and "gc.root" not in ir
                ll = d / (source.stem + ".ll")
                ll.write_text(ir)
                binary = d / source.stem
                built = run([
                    args.clang, "-O1", "-x", "ir", str(ll),
                    "-x", "c", "-std=c11", "-I", str(runtime.parents[2]),
                    str(runtime), "-o", str(binary)
                ], timeout=65)
                assert built.returncode == 0, (source.name, built.stderr)
                result = run([str(binary)])
                assert result.returncode == 42 and not result.stderr, (
                    source.name, result.returncode, result.stderr)
                count += 1
    print(f"PASS: {count} native CFG source programs, reverse cleanup on every exit")


if __name__ == "__main__":
    main()
