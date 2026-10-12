#!/usr/bin/env python3
"""CTest harness: verify safe rejection, verified IR, and optional native execution."""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile



def verify_three_actor_pipeline_stderr(stderr):
    """Require observed 3-actor guest state, finalizers and physical group close.

    An aggregate marker is NOT a replayable event trace. A trusted CI runner
    must still build and execute this exact source with its actual native ABI.
    """
    lines = stderr.splitlines()
    marker = "SOURCE_PIPELINE_ROUNDTRIP_PASS actors=3 typed=1 forward=2 backward=2 result=19"
    assert lines.count(marker) == 1, "missing or duplicated compiled roundtrip"
    assert not any(line.startswith((
        "SOURCE_PIPELINE_FAILURE", "SOURCE_PIPELINE_TERMINAL_STATUS",
        "SOURCE_ACTOR_INVARIANT_FAILURE"
    )) for line in lines), "guest pipeline reported terminal failure"

    stop_prefix = "SOURCE_ACTOR_SPAWN_STOP_VERIFIED "
    stops = [line for line in lines if line.startswith(stop_prefix)]
    expected = [
        stop_prefix + "constructor_on_worker=1 released=1 processed=1 last=13",
        stop_prefix + "constructor_on_worker=1 released=1 processed=2 last=14",
        stop_prefix + "constructor_on_worker=1 released=1 processed=2 last=19",
    ]
    assert stops == expected, "physical actor finalizers/state mismatch"
    groups = [line for line in lines if line.startswith("SOURCE_ACTOR_GROUP_VERIFIED ")]
    assert groups == ["SOURCE_ACTOR_GROUP_VERIFIED active=0 slots=3 peak=3 carriers=4"], (
        "final physical ActorGroup barrier missing or duplicated"
    )

def verify_three_actor_flow_context(stderr):
    """Require actual native admission provenance, not a success substring.

    All five directional messages must carry the same host-issued flow ID,
    unique message IDs, a strict parent chain, and an unambiguous target
    role. This is a single-flow guest proof, NOT multi-flow conformance.
    """
    prefix = "SOURCE_PIPELINE_CONTEXT_ADMIT "
    rows = [line for line in stderr.splitlines() if line.startswith(prefix)]
    assert len(rows) == 5, "expected five real scheduler DATA context commits"
    pattern = re.compile(
        r"SOURCE_PIPELINE_CONTEXT_ADMIT flow=(\d+) message=(\d+) "
        r"parent=(\d+) target=(\d+) direction=(\d+) "
        r"domain=(\d+) generation=(\d+)"
    )
    parsed = []
    for row in rows:
        match = pattern.fullmatch(row)
        assert match is not None, "malformed host-issued context record"
        parsed.append(tuple(map(int, match.groups())))
    flow_id = parsed[0][0]
    assert flow_id > 0
    assert [(r[3], r[4]) for r in parsed] == [
        (0, 0), (1, 1), (2, 1), (1, 2), (0, 2)
    ], "compiled guest route used a wrong target or direction"
    assert all(r[0] == flow_id for r in parsed), "cross-flow DATA contamination"
    assert len({r[1] for r in parsed}) == 5
    assert all(r[1] > 0 and r[5] > 0 and r[6] > 0 for r in parsed), (
        "stale/zero message identity, domain, or incarnation"
    )
    assert len({r[5] for r in parsed}) == 1, "cross-domain DATA route"
    assert parsed[0][2] == 0, "external source ingress has a forged parent"
    assert all(parsed[i][2] == parsed[i - 1][1] for i in range(1, 5)), (
        "broken runtime-owned causal chain"
    )


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
    parser.add_argument("--pipeline-three-actor-summary", action="store_true")
    parser.add_argument("--pipeline-context-summary", action="store_true")
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
            if args.pipeline_three_actor_summary:
                assert pathlib.Path(args.fixture).name == "source-actor-pipeline-bidirectional.ores", (
                    "three-actor summary gate requires the canonical compiled fixture"
                )
                assert args.run == 43 and args.source_actor_runtime, (
                    "three-actor summary gate requires native compiled actor execution"
                )
                verify_three_actor_pipeline_stderr(result.stderr)
            if args.pipeline_context_summary:
                assert args.pipeline_three_actor_summary, (
                    "context gate requires full physical guest transcript gate"
                )
                verify_three_actor_flow_context(result.stderr)


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"contract test failed: {error}", file=sys.stderr)
        sys.exit(1)
