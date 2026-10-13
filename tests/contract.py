#!/usr/bin/env python3
"""CTest harness: verify safe rejection, verified IR, and optional native execution."""
import argparse
import pathlib
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
    parser.add_argument("--clang")
    parser.add_argument("--target")
    parser.add_argument("--native-runtime")
    parser.add_argument("--experimental-owned-int", action="store_true")
    parser.add_argument("--no-native-abi", action="store_true")
    parser.add_argument("--no-main-link", action="store_true")
    args = parser.parse_args()
    cmd = [args.compiler]
    if args.target:
        cmd.append("--target=" + args.target)
    if args.experimental_owned_int:
        cmd.append("--experimental-owned-int")
    cmd.append(args.fixture)
    compile_result = subprocess.run(
        cmd, capture_output=True, text=True, check=False, timeout=20
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
    if args.no_native_abi:
        assert "oreslang_owned_i64_region_" not in compile_result.stdout, (
            "ordinary source unexpectedly references the experimental native heap ABI"
        )
    if args.no_main_link:
        assert args.run is None, "--no-main-link cannot request successful execution"
        assert args.clang, "--no-main-link requires clang"
        assert "define i32 @main()" not in compile_result.stdout, (
            "library-only source unexpectedly received a native OS entrypoint"
        )
        with tempfile.TemporaryDirectory(prefix="oreslang-missing-main-") as directory:
            executable = str(pathlib.Path(directory) / "guest")
            link = subprocess.run(
                [args.clang, "-x", "ir", "-", "-o", executable],
                input=compile_result.stdout, capture_output=True,
                text=True, check=False, timeout=30,
            )
            assert link.returncode != 0, (
                "source with no main() linked as a successful standalone process"
            )
        return
    if args.run is None:
        return
    assert args.clang, "--run requires a clang executable"
    with tempfile.TemporaryDirectory(prefix="oreslang-llvm-test-") as directory:
        executable = str(pathlib.Path(directory) / "guest")
        clang_cmd = [args.clang, "-x", "ir", "-"]
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
        # Only a checked i32 OS wrapper may produce the process status.
        # Raw i64 guest results must never rely on platform truncation.
        assert "define i32 @main()" in compile_result.stdout, (
            "native executable lacks an i32 main entrypoint wrapper"
        )
        assert result.returncode == args.run, (
            f"native exit {result.returncode}, expected {args.run}: {result.stderr}"
        )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
