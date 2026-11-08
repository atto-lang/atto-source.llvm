#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations."""
import argparse
import pathlib
import subprocess
import tempfile

def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=60, **kwargs)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--fixture", required=True)
    p.add_argument("--harness", required=True)
    args = p.parse_args()
    compiler = args.compiler
    fixture = pathlib.Path(args.fixture)
    cases = [
        ("val count: int = 0;", "val count: int = -1;"),
        ("count = count + 1;", "count = count + 2;"),
        ("while (count < 1000)", "while (count < 2000000000)"),
        ("return count;", "return 17;"),
        ("val count: int = 0;", "const count: int = 0;"),
        ("count = count + 1;", "other = other + 1;"),
        ("while (count < 1000)", "while (count < 1000) { count = count + 1; } while (count < 2000)"),
    ]
    source = fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="oreslang-rt-frame-") as folder:
        out = pathlib.Path(folder)
        def compile_source(path, gated=True):
            return run([compiler] + (["--experimental-rt-scalar-loop"] if gated else []) + [str(path)])
        denied = compile_source(fixture, False)
        assert denied.returncode != 0 and not denied.stdout.strip(), "default loop gate reopened"
        assert "requires verified LLVM continuation lowering" in denied.stderr, denied.stderr
        accepted = compile_source(fixture)
        assert accepted.returncode == 0, accepted.stderr
        assert "define i32 @oreslang_rt_scalar_frame_step" in accepted.stdout, "not compiled guest step"
        assert "define void @oreslang_rt_scalar_frame_cancel" in accepted.stdout
        assert "define void @oreslang_rt_scalar_frame_init" in accepted.stdout
        assert "define i64 @oreslang_rt_scalar_frame_result" in accepted.stdout
        # The LLVM source emitter must prove a single batch-fuel admission:
        # decrement/compare of dispatch fuel on each loop backedge is forbidden.
        assert "batch.requested_end" in accepted.stdout
        assert "batch.limit" in accepted.stdout
        assert "batch.current" in accepted.stdout
        assert "fuel.remaining" not in accepted.stdout
        assert "dispatch.fuel" not in accepted.stdout
        llvm_ir = out / "guest.ll"
        llvm_ir.write_text(accepted.stdout)
        # Run the *same* compiled source guest under unoptimized and two
        # optimized native pipelines. A checkpoint/fuel mutation that changes
        # behavior at O2/O3 must not be masked by a single debug result.
        expected_receipts = (
            "SCALAR_FRAME_AOT_TWO_TASKS_PASS",
            "SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS",
            "SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS",
            "SCALAR_FRAME_AOT_BATCH_FUEL_BOUNDARIES_PASS",
        )
        for opt in ("-O0", "-O2", "-O3"):
            exe = out / ("run-guest-" + opt[1:])
            build = run([args.clang, "-std=c11", opt, "-DNDEBUG", "-Wall", "-Wextra",
                         "-Werror", "-Wno-error=override-module", "-pedantic",
                         str(args.harness), "-x", "ir", str(llvm_ir), "-o", str(exe)])
            assert build.returncode == 0, (opt, build.stderr)
            execution = run([str(exe)])
            assert execution.returncode == 0, (opt, execution.stderr)
            for receipt in expected_receipts:
                assert receipt in execution.stdout, (opt, receipt, execution.stdout)
        for i, (original, replacement) in enumerate(cases):
            assert original in source
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            assert wrong.returncode != 0 and not wrong.stdout.strip(), (i, wrong.stdout)
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        assert missing.returncode != 0 and not missing.stdout.strip()
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: O0/O2/O3 batched fuel, 14 invalid frames, 7 boundaries, peer/cancel isolation, negatives")

if __name__ == "__main__":
    main()
