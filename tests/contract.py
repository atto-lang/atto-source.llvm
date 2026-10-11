#!/usr/bin/env python3
"""CTest harness: verify safe rejection, verified IR, and optional native execution."""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--ir")
    parser.add_argument("--diagnostic")
    parser.add_argument("--run", type=int)
    parser.add_argument("--entry-symbol")
    parser.add_argument("--clang")
    parser.add_argument("--native-runtime")
    args = parser.parse_args()
    compile_result = subprocess.run(
        [args.compiler, args.fixture], capture_output=True, text=True, check=False, timeout=20
    )
    if args.diagnostic:
        assert compile_result.returncode != 0, "invalid source unexpectedly accepted"
        assert args.diagnostic in compile_result.stderr, compile_result.stderr
        assert not compile_result.stdout.strip(), "emitted IR after rejection"
        return
    assert compile_result.returncode == 0, compile_result.stderr
    assert compile_result.stdout.startswith("; ModuleID"), compile_result.stdout
    assert not compile_result.stderr, compile_result.stderr
    if args.ir:
        assert args.ir in compile_result.stdout, compile_result.stdout
    if args.run is None:
        return
    assert args.clang, "--run requires a clang executable"
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-test-") as directory:
        executable = str(pathlib.Path(directory) / "guest")
        clang_cmd = [args.clang, "-x", "ir", "-"]
        if args.entry_symbol:
            if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.]*", args.entry_symbol):
                raise AssertionError("unsafe native qualified entrypoint name")
            # This test-only shim invokes a module-qualified LLVM symbol
            # without claiming that LLVM has a public unqualified OS main ABI.
            # The compiler emitted IR and the actual function body both run.
            shim = pathlib.Path(directory) / "module-entry.c"
            shim.write_text(
                'extern long long oreslang_module_entry(void) __asm__("'
                + args.entry_symbol + '");\n'
                + 'int main(void) { return (int)oreslang_module_entry(); }\n',
                encoding="utf-8",
            )
            clang_cmd.extend(["-x", "c", str(shim), "-std=c11"])
        if args.native_runtime:
            native_source = pathlib.Path(args.native_runtime)
            clang_cmd.extend(["-x", "c", str(native_source), "-std=c11",
                              "-I", str(native_source.parents[2])])
        clang_cmd.extend(["-o", executable])
        native = subprocess.run(
            clang_cmd, input=compile_result.stdout, capture_output=True,
            text=True, check=False, timeout=30
        )
        assert native.returncode == 0, native.stderr
        result = subprocess.run(
            [executable], capture_output=True, text=True, check=False, timeout=10
        )
        # The native platform truncates main's i64 return to its process exit code.
        assert result.returncode == args.run, (
            f"native exit {result.returncode}, expected {args.run}: {result.stderr}"
        )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
