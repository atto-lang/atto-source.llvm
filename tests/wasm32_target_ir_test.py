#!/usr/bin/env python3
"""Tests the exact output of native compiler's experimental wasm32 IR switch."""
import subprocess
import sys
from pathlib import Path

compiler = sys.argv[1]
source = sys.argv[2]
normal = subprocess.run([compiler, source], capture_output=True, text=True)
assert normal.returncode == 0, normal.stderr
assert 'target triple = "wasm32-unknown-unknown"' not in normal.stdout
wasm = subprocess.run([compiler, "--target=wasm32", source], capture_output=True, text=True)
assert wasm.returncode == 0, wasm.stderr
assert 'target triple = "wasm32-unknown-unknown"' in wasm.stdout
assert 'target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"' in wasm.stdout
assert "define i64 @main" in wasm.stdout
for flag in ("--target=wasm32", "--does-not-exist"):
    invalid = subprocess.run([compiler, flag, flag, source], capture_output=True, text=True)
    assert invalid.returncode != 0 and not invalid.stdout
print("wasm32 LLVM IR switch and negative flags verified")
