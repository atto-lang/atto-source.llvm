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
    parser.add_argument("--native-runtime")
    parser.add_argument("--actor-runtime")
    parser.add_argument("--native-harness")
    parser.add_argument("--scheduler-runtime")
    parser.add_argument("--source-actor-runtime")
    parser.add_argument("--stderr")
    parser.add_argument("--stderr-count", type=int)
    parser.add_argument("--runs", type=int, default=1)
    parser.add_argument("--sanitize", action="store_true")
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
        clang_args = [args.clang, "-x", "ir", "-"]
        if args.sanitize:
            clang_args.extend(["-fsanitize=address,undefined", "-fno-omit-frame-pointer"])
        if args.native_runtime:
            native_source = pathlib.Path(args.native_runtime)
            clang_args.extend([
                "-x", "c", str(native_source),
                "-std=c11", "-I", str(native_source.parents[2]),
            ])
        if args.actor_runtime:
            path = pathlib.Path(args.actor_runtime)
            clang_args.extend([
                "-x", "c", str(path), "-std=c11",
                "-I", str(path.parents[2]),
            ])
        if args.scheduler_runtime:
            path = pathlib.Path(args.scheduler_runtime)
            clang_args.extend(["-x", "c", str(path), "-std=c11",
                               "-I", str(path.parents[2]), "-pthread"])
        if args.source_actor_runtime:
            path = pathlib.Path(args.source_actor_runtime)
            native_future = path.parent / "NativeActorFuture.c"
            native_messagepack = path.parent / "MessagePackActorV2.c"
            clang_args.extend(["-x", "c", str(path), str(native_future),
                               str(native_messagepack), "-std=c11",
                               "-DORESLANG_SOURCE_ACTOR_TESTING",
                               "-I", str(path.parents[2]), "-pthread"])
        if args.native_harness:
            harness = pathlib.Path(args.native_harness)
            # The actor source module may contain no top-level main;
            # the trusted test harness supplies native actor turns.
            clang_args.extend(["-x", "c", str(harness), "-std=c11"])
        clang_args.extend(["-o", executable])
        native = subprocess.run(
            clang_args, input=compile_result.stdout,
            capture_output=True, text=True, check=False, timeout=30
        )
        assert native.returncode == 0, native.stderr
        assert 1 <= args.runs <= 100, "unreasonable repeat count"
        for trial in range(args.runs):
            result = subprocess.run(
                [executable], capture_output=True, text=True, check=False, timeout=10
            )
            # The native platform truncates main's i64 return to its exit code.
            assert result.returncode == args.run, (
                f"trial {trial}: native exit {result.returncode}, "
                f"expected {args.run}: {result.stderr}"
            )
            if args.stderr:
                assert args.stderr in result.stderr, result.stderr
            if args.stderr_count is not None:
                assert args.stderr, "--stderr-count needs --stderr"
                assert result.stderr.count(args.stderr) == args.stderr_count, (
                    f"trial {trial}: {result.stderr}"
                )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
