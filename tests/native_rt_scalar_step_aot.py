#!/usr/bin/env python3
"""Compile actual opt-in Oreslang source and drive two native heap continuations.

Verification guards remain active with PYTHONOPTIMIZE/-O and Clang -DNDEBUG.
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

def run(command, **kwargs):
    return subprocess.run(command, text=True, capture_output=True, check=False,
                          timeout=60, **kwargs)


def require_generated_loop_contract(ir):
    """Inspect the actual opt-in compiler's pre-optimization native IR.

    Only scalar .ores frames are admitted here. Hard fuel is charged once per
    dispatch into a precomputed batch endpoint (main's batched lowering);
    costly host scheduler/clock work MUST remain outside the compiled loop.
    This structural receipt complements, never replaces, native execution.
    """
    header = re.search(r"define i32 @oreslang_rt_scalar_frame_step\(ptr (%[\w.]+), i64 (%[\w.]+)\)", ir)
    if header is None or ir.count("define i32 @oreslang_rt_scalar_frame_step(") != 1:
        raise AssertionError("missing or duplicate compiled step symbol")
    fuel_param = header.group(2)
    rest = ir[header.end():]
    if "\n}" not in rest:
        raise AssertionError("malformed emitted step function")
    step = rest.split("\n}", 1)[0]
    blocks = {}
    current = None
    for line in step.splitlines():
        label = re.match(r"^([a-zA-Z][a-zA-Z0-9_.]*):", line)
        if label:
            current = label.group(1)
            if current in blocks:
                raise AssertionError("duplicate native CFG block: " + current)
            blocks[current] = []
        elif current is not None:
            blocks[current].append(line)
    needed = ("counter.valid", "loop.check", "loop.commit", "loop.batch.exit", "cooperate")
    if any(name not in blocks for name in needed):
        raise AssertionError("missing compiler-generated bounded-loop CFG blocks")
    commit = "\n".join(blocks["loop.commit"])
    check = "\n".join(blocks["loop.check"])
    endpoint = "\n".join(blocks["counter.valid"])
    hot = check + "\n" + commit
    # Never invoke a scheduler, watchdog, clock, allocation or host FFI from
    # a compiled scalar backedge. The only hot work is bounded arithmetic,
    # branch and committed frame effects.
    if re.search(r"\b(?:call|invoke)\b", hot):
        raise AssertionError("expensive slow path in compiler-generated loop")
    if not re.search(r"store i64 [^\n]+", commit):
        raise AssertionError("missing committed guest frame store")
    if "br label %loop.check" not in commit:
        raise AssertionError("missing compiled bounded-loop backedge")
    if "%dispatch.batch.stop" not in check:
        raise AssertionError("loop bound does not use the batched dispatch endpoint")
    # The whole dispatch allowance is charged exactly once, before the loop.
    if not re.search(r"%dispatch\.end\.by\.fuel = add i64 %[\w.]+, " + re.escape(fuel_param) + r"\b", endpoint):
        raise AssertionError("missing exact hard-fuel charge into the batch endpoint")
    if len(re.findall(r"icmp ule i64 [^,\n]+, 512\b", step)) != 1:
        raise AssertionError("missing exact 512-unit native admission cap")


def prove_structural_guard_catches_mutations(ir):
    """These are *structural* mutations, separate from executable mutants."""
    cap = re.sub(r"icmp ule i64 ([^,\n]+), 512\b",
                 r"icmp ule i64 \g<1>, 4096", ir, count=1)
    missing_charge = re.sub(r"(%dispatch\.end\.by\.fuel = add i64 %[\w.]+), %[\w.]+",
                            r"\g<1>, 1000", ir, count=1)
    # A third mutation simulates accidental expensive host polling on every
    # edge. This is deliberately not compiled; structural rejection is the
    # evidence category, rather than falsely claiming a killed native guest.
    injected_slow_path = re.sub(r"^loop\.commit:([^\n]*)",
                                r"loop.commit:\g<1>\n  call void @untrusted_slow_poll()",
                                ir, count=1, flags=re.MULTILINE)
    for label, mutated in (("fuel-cap", cap),
                           ("hard-fuel", missing_charge),
                           ("every-edge-slow-poll", injected_slow_path)):
        if mutated == ir:
            raise AssertionError("structural mutation did not change IR: " + label)
        try:
            require_generated_loop_contract(mutated)
        except AssertionError:
            continue
        raise AssertionError("IR structural guard admitted mutation: " + label)

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
        # Generated IR contract: the source loop has a single source-bound
        # branch and a precomputed dispatch endpoint, NOT a fuel predicate
        # executed on every committed backedge.
        if "dispatch.batch.stop" not in accepted.stdout:
            raise AssertionError("missing batched endpoint")
        if "loop.batch.exit" not in accepted.stdout:
            raise AssertionError("missing batch exit")
        if "loop.fuel" in accepted.stdout:
            raise AssertionError("per-backedge fuel test regressed")
        if "fuel.remaining" in accepted.stdout:
            raise AssertionError("per-iteration fuel decrement regressed")
        if "counter.valid" not in accepted.stdout:
            raise AssertionError("corrupted frame counter guard missing")
        require_generated_loop_contract(accepted.stdout)
        prove_structural_guard_catches_mutations(accepted.stdout)
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
        if not ("SCALAR_FRAME_AOT_COMPILED_BATCHED_QUANTA_PASS" in execution.stdout):
            raise AssertionError("compiler-native batch handoffs not executed")
        if not ("SCALAR_FRAME_AOT_HOST_OWNED_LIFETIME_QUOTA_PASS" in execution.stdout):
            raise AssertionError("host lifetime fuel isolation not executed")
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
                "charge-elision",
                r"^(\s*%dispatch\.end\.by\.fuel = add i64 %[\w.]+, )%[\w.]+(\s*)$",
                r"\g<1>1000\g<2>"),
        ]
        # The harness exercises the forbidden 513-unit dispatch only through
        # its Linux fork/trap children, so the cap mutant is killable there.
        if sys.platform.startswith("linux"):
            killed_mutants.append(require_killed_guest_mutant(
                "fuel-cap",
                r"^(\s*%[\w.]+ = icmp ule i64 [^,\n]+, )512(\s*)$",
                r"\g<1>4096\g<2>"))
        if not (len(killed_mutants) == (2 if sys.platform.startswith("linux") else 1)):
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
        print("NATIVE_OPT_IN_SCALAR_FRAME_PASS: compiled 64/128/256/512 dispatch quanta, host lifetime quota, 3 peer frames, killed native IR mutations, 3 killed structural mutations, negatives")

if __name__ == "__main__":
    main()
