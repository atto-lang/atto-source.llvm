#!/usr/bin/env python3
"""Standalone native compiler env/argv precedence and validation regression."""
import argparse
import os
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--source", required=True)
args = parser.parse_args()

base = {k: v for k, v in os.environ.items() if not k.startswith("ORES_LLVM_")}

def run(options, overrides=None):
    env = dict(base)
    env.update(overrides or {})
    return subprocess.run([args.compiler, *options, args.source],
                          env=env, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, text=True, timeout=20)

def expect(status, options, overrides=None, stderr_text=None):
    result = run(options, overrides)
    if result.returncode != status or (stderr_text and stderr_text not in result.stderr):
        raise AssertionError(
            f"expected status={status} for {options} {overrides}, got "
            f"{result.returncode}\nstdout={result.stdout[:600]}\nstderr={result.stderr[:600]}")
    return result

host = expect(0, [])
assert "define" in host.stdout
expect(0, ["--target=host"], {"ORES_LLVM_TARGET": "wasm32"})
expect(0, [], {"ORES_LLVM_EXPERIMENTAL_OWNED_INT": "false"})
expect(0, ["--no-experimental-owned-int"], {"ORES_LLVM_EXPERIMENTAL_OWNED_INT": "true"})
expect(2, [], {"ORES_LLVM_EXPERIMENTAL_OWNED_INT": "not-a-bool"}, "invalid boolean")
expect(2, [], {"ORES_LLVM_CPU": "avx512"}, "not implemented")
expect(2, [], {"ORES_LLVM_TARGET": "unverified"}, "unknown target")
expect(2, ["--experimental-owned-int", "--experimental-owned-int"], {}, "duplicate")
expect(2, ["--target=host", "--target=host"], {}, "duplicate")
print("LLVM flags-2-env / argv precedence regression passed")
