#!/usr/bin/env python3
"""Reject unsupported async I/O bindings until a real native reactor exists.

This test is deliberately future-hostile: adding names without typed effects,
continuation lowering and a nonblocking transport must not silently pass.
"""
import pathlib
import subprocess
import sys
import tempfile

if len(sys.argv) != 2:
    raise SystemExit("usage: native_async_io_boundary_matrix.py <compiler>")

compiler = sys.argv[1]
# The native compiler currently supports int-only bare function calls.
# A native I/O binding with these names must never be accepted merely because
# the name ends with "_async" or "_blocking".
cases = {
    "read blocking": "read_text_blocking(1)",
    "read async": "read_text_async(1)",
    "write blocking": "write_text_blocking(1)",
    "write async": "write_text_async(1)",
    "TCP connect blocking": "connect_blocking(1)",
    "TCP connect async": "connect_async(1)",
    "TCP read blocking": "read_line_blocking(1)",
    "TCP read async": "read_line_async(1)",
    "TCP write blocking": "socket_write_text_blocking(1)",
    "TCP write async": "socket_write_text_async(1)",
    "HTTP GET blocking": "http_get_text_blocking(1)",
    "HTTP GET async": "http_get_text_async(1)",
    "HTTP POST blocking": "http_post_text_blocking(1)",
    "HTTP POST async": "http_post_text_async(1)",
    "stdio blocking": "println_blocking(1)",
    "stdio async": "println_async(1)",
}
with tempfile.TemporaryDirectory(prefix="oreslang-async-io-boundary-") as root:
    file = pathlib.Path(root) / "probe.ores"
    for label, expression in cases.items():
        file.write_text(f"pub fnc main(): int {{ return {expression}; }}\n")
        result = subprocess.run([compiler, str(file)], capture_output=True,
                                text=True, timeout=15, check=False)
        assert result.returncode != 0, (
            label, "UNSAFE: unsupported I/O compiled", result.stdout)
        assert not result.stdout.strip(), (
            label, "UNSAFE: LLVM IR emitted for an unsupported I/O operation",
            result.stdout)
        assert "unknown function or wrong call arity" in result.stderr, (
            label, result.stderr)
print(f"Native async-only I/O boundary: {len(cases)} unsupported native bindings fail closed")
