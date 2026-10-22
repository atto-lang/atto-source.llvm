#!/usr/bin/env python3
"""Compile real source actor LLVM IR and execute 10k admitted native turns."""
import argparse
import os
from pathlib import Path
import resource
import signal
import subprocess
import tempfile


def run(argv, *, timeout=120, env=None):
    return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=timeout, env=env,
                          preexec_fn=lambda: resource.setrlimit(
                              resource.RLIMIT_CORE, (0, 0)))


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", required=True)
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--sanitize", action="store_true")
    args = p.parse_args()
    root = Path(args.root).resolve()
    fixture = root / "tests/fixtures/source-actor-owned-drop-turn-stress.ores"
    header_root = root / "src/main/cpp"
    native_heap = header_root / "oreslang/runtime/ActorOwnedI64Heap.c"
    driver = root / "tests/source_actor_owned_drop_10000.c"

    compiled = run([args.compiler, "--experimental-owned-int", str(fixture)])
    require(compiled.returncode == 0,
            "native source actor compiler failed: " + compiled.stderr)
    ir = compiled.stdout
    require("define void @oreslang_actor_Worker_init(" in ir,
            "missing generated native source constructor")
    require("define i64 @oreslang_actor_Worker_on_message(" in ir,
            "missing generated actor message handler")
    new_token = "call i32 @ores_actor_i64_heap_scoped_try_new("
    drop_token = "call void @ores_actor_i64_heap_scoped_drop("
    require(ir.count(new_token) == 2 and ir.count(drop_token) == 3,
            "missing checked owner acquisition or partial/normal drop calls")
    require("actor.owned.exhausted:" in ir,
            "missing compiler-generated native allocation failure edge")
    require("gc.statepoint" not in ir and "gc.root" not in ir,
            "unexpected LLVM tracing GC root intrinsics")

    with tempfile.TemporaryDirectory(prefix="ores-native-actor-owned-stress-") as temp:
        work = Path(temp)
        env = os.environ.copy()
        if args.sanitize:
            env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
            env["UBSAN_OPTIONS"] = "halt_on_error=1"
        flags = ["-O1", "-g", "-fno-omit-frame-pointer"] if args.sanitize else ["-O2"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined"]

        def linked(name, source_ir):
            ll = work / (name + ".ll")
            binary = work / name
            ll.write_text(source_ir)
            command = [args.clang, *flags, "-x", "ir", str(ll),
                       "-x", "c", "-std=c11", "-pthread",
                       "-I", str(header_root), str(native_heap), str(driver),
                       "-o", str(binary)]
            built = run(command)
            require(built.returncode == 0,
                    "native actor source link failed (" + name + "): " + built.stderr)
            return binary

        live = linked("actor-owned-10000", ir)
        result = run([str(live)], env=env)
        marker = "SOURCE_ACTOR_COMPILER_DROP_10000_TURNS_PASS"
        require(result.returncode == 0 and
                result.stdout.splitlines().count(marker) == 1,
                "10k native actor turns failed: exit=" +
                str(result.returncode) + " stdout=" + result.stdout +
                " stderr=" + result.stderr)

        # Mutation adequacy: remove *all three* compiler-owned drop
        # instructions (including one partial-failure cleanup edge) from otherwise unchanged valid LLVM IR, rebuild the
        # same source handler and require deterministic native fail-stop.
        dropped = [line for line in ir.splitlines(keepends=True)
                   if drop_token in line]
        require(len(dropped) == 3, "mutation found unexpected drop instruction count")
        mutant_ir = "".join(line for line in ir.splitlines(keepends=True)
                            if drop_token not in line)
        mutant = linked("actor-owned-missing-drops", mutant_ir)
        denied = run([str(mutant)], env=env)
        require(denied.returncode == -signal.SIGABRT,
                "removing compiler drops was not caught by bounded native heap: " +
                str(denied.returncode) + " " + denied.stderr)
        require(marker not in denied.stdout, "drop-less mutant wrongly passed")

    print("PASS: actual Oreslang source actor compiled native, 10000 admitted turns,"
          " owned drops exactly twice/turn, no-drop mutation SIGABRT,"
          " sanitized=" + str(args.sanitize))


if __name__ == "__main__":
    main()
