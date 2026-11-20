#!/usr/bin/env python3
"""Native compiler negative admission for independently authored AttoXP slices.

This is a real compiler invocation. No successful IR/stdout is allowed.
"""
import argparse
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
NEGATIVE = ("unsupported-op.atto", "unknown-call.atto", "bad-arity.atto")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    if not compiler.is_file() or args.compiler.is_symlink():
        raise SystemExit("missing or symlinked native compiler")
    for name in NEGATIVE:
        path = ROOT / "contracts" / "attoxp-counter-v1" / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit("missing or linked fixture " + name)
        outcome = subprocess.run(
            [str(compiler), str(path)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=8,
            check=False,
        )
        if not (outcome.returncode > 0 and outcome.stdout == b""
                and 0 < len(outcome.stderr) <= 65536):
            raise SystemExit("LLVM compiler unexpectedly admitted " + name)
    print("ATTOXP_LLVM_COUNTER_NEGATIVE:3")

if __name__ == "__main__":
    main()
