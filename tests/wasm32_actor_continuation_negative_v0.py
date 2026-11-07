#!/usr/bin/env python3
"""Compiler-backed negative admission: no wasm32 actor guest suspension ABI.

The suite invokes the actual native oreslang-llvmc at its checkout HEAD.
A rejected flag or source must produce no LLVM IR artifact. Successful
checked synchronous bounded recursion is a separate positive baseline.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--negative", required=True)
parser.add_argument("--baseline", required=True)
args = parser.parse_args()
compiler = Path(args.compiler).resolve(strict=True)
negative = Path(args.negative).resolve(strict=True)
baseline = Path(args.baseline).resolve(strict=True)

def blob(data: bytes) -> str:
    return hashlib.sha1(f"blob {len(data)}\0".encode() + data).hexdigest()

assert negative.suffix == baseline.suffix == ".atto"
assert blob(negative.read_bytes()) == "aebd40549e88b49c906fd3fa2b0569ec7800deec"
assert blob(baseline.read_bytes()) == "947781cddf09d8e3e08c51cff2b7d8960e2d4750"

with tempfile.TemporaryDirectory(prefix="atto-actor-continuation-closed-") as tmp:
    scratch = Path(tmp)

    def invoke(options, source):
        return subprocess.run(
            [str(compiler), *options, str(source)],
            capture_output=True, text=True, timeout=60, check=False,
            cwd=scratch
        )

    def must_reject(title, options, source, diagnostic=None):
        before = sorted(scratch.iterdir())
        run = invoke(options, source)
        assert run.returncode != 0, (title, "UNSAFE ADMITTED", run.stdout[:300])
        assert run.stdout == "", (title, "rejected source emitted LLVM IR")
        assert run.stderr.strip(), (title, "no source admission diagnostic")
        if diagnostic is not None:
            assert diagnostic in run.stderr, (title, run.stderr[:900])
        assert sorted(scratch.iterdir()) == before, (
            title, "rejected source published artifact to caller workspace")

    for flag in ("--experimental-wasm-actor-continuations-v0",
                 "--experimental-wasm-actor-suspend-v0"):
        for options in (
            ["--target=wasm32", flag],
            ["--target=wasm32", "--experimental-wasm-actor-recursion-v1", flag],
            [flag, "--target=wasm32", "--experimental-wasm-actor-recursion-v1"],
            ["--target=host", flag]
        ):
            must_reject(
                "unsupported continuation flag " + repr(options), options, baseline,
                "wasm32 actor continuations are NOT IMPLEMENTED")

    # The negative AttoLang fixture includes a source-level suspends fnc.
    # It must not slip through either the ordinary wasm32 actor backend or
    # the special verified pure-recursion lowering. Syntax-level rejection
    # is acceptable here because source suspension has no guest ABI.
    must_reject("unflagged suspended actor",
                ["--target=wasm32"], negative)
    must_reject("recursive mode may not erase suspended helper",
                ["--target=wasm32",
                 "--experimental-wasm-actor-recursion-v1"], negative)

    # Preserve the admitted non-suspending iterative actor recursion
    # without interpreting any guest continuation as synchronous fallback.
    result = invoke(["--target=wasm32",
                     "--experimental-wasm-actor-recursion-v1"], baseline)
    assert result.returncode == 0, ("positive recursion regressed", result.stderr[:900])
    assert 'target triple = "wasm32-unknown-unknown"' in result.stdout
    assert "atto.trampoline.push" in result.stdout
    assert "@oreslang_wasm_actor_frame_status()" in result.stdout
    assert "@descend(" not in result.stdout, "engine-native recursive call leaked"
    assert sorted(scratch.iterdir()) == [], "compiler emitted unrequested files"

print("PASS: explicit guest suspension/continuation admission denied; synchronous actor recursion unaffected")
