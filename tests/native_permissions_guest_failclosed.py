#!/usr/bin/env python3
"""Compile a real .atto positive control and reject unsupported native host I/O.

This is a fail-closed guest-source admission gate, NOT a permissions parity
report. The current compiler has no runtime-scoped file/env/net/process/FFI
lowering; future support must replace these negatives with real authorization
tests instead of weakening this gate or claiming all 52 vectors executed.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

# Unknown guest host-I/O entrypoints must not become accidental compiler
# intrinsics / imports that bypass the privileged broker or scoped policy.
DENIED_GUEST_CALLS = (
    ("filesystem-read", 'read_file("/etc/passwd")'),
    ("filesystem-write", 'write_file("/tmp/atto-forbidden", "x")'),
    ("network-connect", 'connect_tcp("127.0.0.1:443")'),
    ("environment-read", 'get_env("PATH")'),
    ("process-spawn", 'spawn_process("sh")'),
    ("ffi-library", 'load_native_library("/tmp/unknown.so")'),
    ("hot-code-import", 'load_module("/tmp/foreign.atto")'),
)
# No ad-hoc parser may silently accept permissions until flags-2-env and a
# real verified runtime permission manifest are connected to native AOT.
DENIED_UNSUPPORTED_FLAGS = (
    "--allow-all",
    "--allow-read=/etc/passwd",
    "--allow-net=127.0.0.1:443",
    "--allow-ffi=/tmp/unknown.so",
)


def execute(command):
    return subprocess.run(command, stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          timeout=30, check=False)


def require(ok, label, detail=""):
    if not ok:
        raise AssertionError(f"{label}: {detail[:300]}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    clang = args.clang.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="atto-native-permissions-deny-") as tmp:
        root = Path(tmp)
        guest = root / "main.atto"
        llvm = root / "main.ll"
        binary = root / "main"
        # Positive control establishes that this is a working production
        # .atto compiler -> LLVM IR -> native executable path.
        guest.write_text("pub fnc main():int{return 7;}\n", encoding="utf-8")
        positive = execute([str(compiler), str(guest)])
        require(positive.returncode == 0, "positive compiler",
                positive.stderr.decode(errors="replace"))
        require(b"define i32 @main(" in positive.stdout, "missing real OS entrypoint")
        llvm.write_bytes(positive.stdout)
        linked = execute([str(clang), "-O0", "-x", "ir", str(llvm), "-o", str(binary)])
        require(linked.returncode == 0 and binary.is_file(), "positive Clang link",
                linked.stderr.decode(errors="replace"))
        actual = execute([str(binary)])
        require(actual.returncode == 7 and actual.stdout == b"" and
                actual.stderr == b"", "positive native guest execution",
                repr((actual.returncode, actual.stdout, actual.stderr)))
        for name, expression in DENIED_GUEST_CALLS:
            guest.write_text("pub fnc main():int{return " + expression + ";}\n",
                             encoding="utf-8")
            result = execute([str(compiler), str(guest)])
            require(result.returncode != 0 and result.stdout == b"" and
                    result.stderr != b"", "unsupported guest I/O admitted: " + name,
                    repr((result.returncode, result.stdout[:100], result.stderr[:300])))
        guest.write_text("pub fnc main():int{return 7;}\n", encoding="utf-8")
        for flag in DENIED_UNSUPPORTED_FLAGS:
            result = execute([str(compiler), flag, str(guest)])
            require(result.returncode != 0 and result.stdout == b"" and
                    result.stderr != b"", "unknown permission flag admitted: " + flag,
                    repr((result.returncode, result.stdout[:100], result.stderr[:300])))
        print("LLVM_ATTO_REAL_NATIVE_SCALAR_CONTROL_EXECUTED 1/1")
        print(f"LLVM_UNSUPPORTED_GUEST_HOST_IO_REJECTED {len(DENIED_GUEST_CALLS)}/{len(DENIED_GUEST_CALLS)}")
        print(f"LLVM_UNSUPPORTED_PERMISSION_FLAGS_REJECTED {len(DENIED_UNSUPPORTED_FLAGS)}/{len(DENIED_UNSUPPORTED_FLAGS)}")
        print("LLVM_SCOPED_GUEST_PERMISSION_PARITY_NOT_ESTABLISHED")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, AssertionError, subprocess.TimeoutExpired) as error:
        print("LLVM_GUEST_PERMISSION_DENIAL_GATE_FAILED: " + str(error), file=sys.stderr)
        sys.exit(1)
