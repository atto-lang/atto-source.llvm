#!/usr/bin/env python3
"""Execute the real LLVM frontend and inspect the verified generated IR."""
import subprocess
import sys

compiler, fixture = sys.argv[1:]
result = subprocess.run(
    [compiler, "--emit-contract-llvm", fixture],
    check=True, capture_output=True, text=True,
)
ir = result.stdout
assert '%oreslang.string = type {' in ir, ir
assert 'i64 }' in ir, ir
assert '@ClientContract.list_items(' in ir, ir
assert 'declare %oreslang.string' in ir, ir
assert 'define ' not in ir, "header declarations must not pretend to implement RPC"
assert 'ClientContract' not in result.stderr, result.stderr
print("LLVM-verified native String contract signatures (declaration-only): PASS")
