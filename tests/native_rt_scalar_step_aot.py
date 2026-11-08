#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations.

Verification guards remain active with PYTHONOPTIMIZE/-O and Clang -DNDEBUG.
"""
import argparse
import pathlib
import re
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
        # Runtime supervision cannot rescue a compiler that accidentally
        # accepts effects/critical sections in the scalar-only AOT subset.
        # Until CFG exit/ownership instrumentation is proven, fail closed.
        ("count = count + 1;", "count = count + 1; critical { count = count + 1; }"),
        ("count = count + 1;", "count = count + 1; rt cooperate;"),
        ("count = count + 1;", "count = count + 1; count = count + 1;"),
        ("count = count + 1;", "count = count + unknown();"),
        ("count = count + 1;", "count = count + 1; defer { count = count + 1; }"),
    ]
    source = fixture.read_text()
    with tempfile.TemporaryDirectory(prefix="oreslang-rt-frame-") as folder:
        out = pathlib.Path(folder)
        def compile_source(path, gated=True):
            return run([compiler] + (["--experimental-rt-scalar-loop"] if gated else []) + [str(path)])
        denied = compile_source(fixture, False)
        if not (denied.returncode != 0 and not denied.stdout.strip()):
            raise AssertionError("default loop gate reopened")
        if not ("requires verified LLVM continuation lowering" in denied.stderr):
            raise AssertionError(denied.stderr)
        accepted = compile_source(fixture)
        if not (accepted.returncode == 0):
            raise AssertionError(accepted.stderr)
        if not ("define i32 @oreslang_rt_scalar_frame_step" in accepted.stdout):
            raise AssertionError("not compiled guest step")
        if not ("define void @oreslang_rt_scalar_frame_cancel" in accepted.stdout):
            raise AssertionError("native AOT assertion failed")
        if not ("define void @oreslang_rt_scalar_frame_init" in accepted.stdout):
            raise AssertionError("native AOT assertion failed")
        if not ("define i64 @oreslang_rt_scalar_frame_result" in accepted.stdout):
            raise AssertionError("native AOT assertion failed")
        llvm_ir = out / "guest.ll"
        llvm_ir.write_text(accepted.stdout)
        exe = out / "run-guest"
        build = run([args.clang, "-std=c11", "-O2", "-DNDEBUG", "-Wall", "-Wextra",
                     "-Werror", "-Wno-error=override-module", "-pedantic", str(args.harness),
                     "-x", "ir", str(llvm_ir), "-o", str(exe)])
        if not (build.returncode == 0):
            raise AssertionError(build.stderr)
        execution = run([str(exe)])
        if not (execution.returncode == 0):
            raise AssertionError(execution.stderr)
        if not ("SCALAR_FRAME_AOT_COMPILED_FUEL_BOUNDARIES_PASS" in execution.stdout):
            raise AssertionError("native AOT assertion failed")
        if not ("SCALAR_FRAME_AOT_TWO_TASKS_PASS" in execution.stdout):
            raise AssertionError("native AOT assertion failed")
        if not ("SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS" in execution.stdout):
            raise AssertionError("native AOT assertion failed")
        if not ("SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS" in execution.stdout):
            raise AssertionError("native AOT assertion failed")

        # Negative mutation of the ACTUAL source-compiled LLVM IR: each
        # weakened guard must still compile successfully, but the executable
        # optimized native guest must fail the original exact-frame oracle.
        # A text-only source assertion or a non-compiling mutant is NOT a
        # valid killed mutation.
        def require_killed_guest_mutant(label, regex, replacement):
            mutated_ir, mutations = re.subn(regex, replacement, accepted.stdout,
                                              count=1, flags=re.MULTILINE)
            if not (mutations == 1):
                raise AssertionError((label, "missing precise source-IR guard"))
            mutated_file = out / f"mutant-{label}.ll"
            mutated_file.write_text(mutated_ir)
            mutant_bin = out / f"mutant-{label}"
            mutant_build = run([
                args.clang, "-std=c11", "-O2", "-DNDEBUG", "-Wall",
                "-Wextra", "-Werror", "-Wno-error=override-module", "-pedantic",
                str(args.harness), "-x", "ir", str(mutated_file), "-o", str(mutant_bin)
            ])
            if not (mutant_build.returncode == 0):
                raise AssertionError((label, mutant_build.stderr))
            killed = run([str(mutant_bin)])
            if not (killed.returncode != 0):
                raise AssertionError((
                label, "REMOVING A COMPILER GUARD STILL PASSED GUEST EXECUTION"))
            if not ("frame check failed" in killed.stderr):
                raise AssertionError((
                label, "mutation failed outside the native guest oracle",
                killed.stderr))
            return label

        killed_mutants = [
            require_killed_guest_mutant(
                "fuel-cap",
                r"^(\s*%[\w.]+ = icmp ule i64 [^,\n]+, )512(\s*)$",
                r"\g<1>4096\g<2>"),
            require_killed_guest_mutant(
                "charge-elision",
                r"^(\s*%fuel\.remaining = sub i64 [^,\n]+, )1(\s*)$",
                r"\g<1>0\g<2>"),
        ]
        if not (len(killed_mutants) == 2):
            raise AssertionError("native AOT assertion failed")

        for i, (original, replacement) in enumerate(cases):
            if not (original in source):
                raise AssertionError("native AOT assertion failed")
            file = out / f"invalid-{i}.ores"
            file.write_text(source.replace(original, replacement, 1))
            wrong = compile_source(file)
            if not (wrong.returncode != 0 and not wrong.stdout.strip()):
                raise AssertionError((i, wrong.stdout))
        missing = run([compiler, "--experimental-rt-scalar-loop", "--target=wasm32", str(fixture)])
        if not (missing.returncode != 0 and not missing.stdout.strip()):
            raise AssertionError("native AOT assertion failed")
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: 3 peer frames, cancellation isolation, bounded progress, 2 compiled-IR killed mutations, negatives")

if __name__ == "__main__":
    main()
