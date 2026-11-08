#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations."""
import argparse
import pathlib
import re
import subprocess
import tempfile

def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=60, **kwargs)

def mutate_one(ir, label, pattern, replacement):
    # Mutation MUST produce a syntactically valid LLVM program. Otherwise a
    # parser rejection would masquerade as a killed security mutation.
    mutated, count = re.subn(pattern, replacement, ir, count=1, flags=re.MULTILINE)
    if count != 1 or mutated == ir:
        raise AssertionError(f"unable to inject LLVM security mutation {label}")
    return mutated


def require_mutations_rejected(ir, out, clang, harness):
    # Compile and execute each mutated *real guest module*. Every change is
    # selected to be valid IR but violate an independently asserted semantic:
    # the bounded fuel cap, exactly-once commit, persisted continuation, or
    # terminal cancellation classification. These MUST fail the native oracle,
    # not just an IR textual preflight or syntax checker.
    mutations = (
        ("bypass-dispatch-fuel",
         r"^(\s*%batch\.limit\s*=\s*)select [^\n]+$",
         r"\g<1>add i64 0, 1000"),
        ("double-commit",
         r"^(\s*%counter\.committed\s*=\s*add\s+(?:nsw\s+)?i64\s+[^,\n]+,\s*)1(\s*)$",
         r"\g<1>2\g<2>"),
        ("drop-persisted-commit",
         r"^(\s*store i64 )%counter\.committed(, [^\n]+)$",
         r"\g<1>%batch.current\g<2>"),
        ("cancel-status-forgery",
         r"^(\s*)ret i32 2(\s*)$",
         r"\g<1>ret i32 0\g<2>"),
    )
    for label, pattern, replacement in mutations:
        changed = mutate_one(ir, label, pattern, replacement)
        mutant_ir = out / (label + ".ll")
        mutant_exe = out / (label + ".exe")
        mutant_ir.write_text(changed)
        build = run([clang, "-std=c11", "-O2", "-DNDEBUG", "-Wall", "-Wextra",
                     "-Werror", "-Wno-error=override-module", "-pedantic",
                     str(harness), "-x", "ir", str(mutant_ir), "-o", str(mutant_exe)])
        assert build.returncode == 0, (label, "mutated IR did not compile", build.stderr)
        execution = run([str(mutant_exe)])
        assert execution.returncode != 0, (label, "security mutant unexpectedly passed",
                                           execution.stdout)
        # An unrelated launcher/crash failure is not proof the native
        # acceptance oracle caught the changed execution semantics.
        assert "frame check failed at " in execution.stderr, (
            label, "mutation failed without a real native frame invariant",
            execution.returncode, execution.stderr)
        assert "SCALAR_FRAME_AOT_TWO_TASKS_PASS" not in execution.stdout, (
            label, "mutant printed successful exact-guest receipt")
    print("SCALAR_FRAME_AOT_FOUR_REAL_IR_MUTATIONS_REJECTED")


def main():
    # Python -O strips assert statements. Never let a release or wrapper
    # invocation silently turn the security regression into an inert script.
    if not __debug__:
        raise RuntimeError("native AOT security test requires Python assertions enabled")
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
            "SCALAR_FRAME_AOT_INVALID_FRAME_TRAPS_16_PASS",
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
        require_mutations_rejected(accepted.stdout, out, args.clang, args.harness)
        for i, (original, replacement) in enumerate(cases):
            assert original in source
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            assert wrong.returncode != 0 and not wrong.stdout.strip(), (i, wrong.stdout)
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        assert missing.returncode != 0 and not missing.stdout.strip()
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: O0/O2/O3 batched fuel, 4 compiled IR mutants, 16 invalid frames, 7 boundaries, peer/cancel isolation, negatives")

if __name__ == "__main__":
    main()
