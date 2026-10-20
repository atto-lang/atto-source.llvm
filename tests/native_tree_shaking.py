#!/usr/bin/env python3
"""Executed proof for semantic/native runtime tree shaking.

The compiler must preserve #693's main/pub/explicit-root behavior while also
following overload slots, actor/class edges, and retained native runtime
features. Dead source cannot hide target-invalid code because the full source
is codegen-verified before deletion.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def require(value, message):
    if not value:
        raise RuntimeError(message)


def run(argv):
    return subprocess.run(
        argv, stdin=subprocess.DEVNULL, capture_output=True,
        timeout=40, check=False
    )


SOURCE = """pub fnc main(): int {
  return live(40);
}
fnc live(int value): int {
  return value + 2;
}
fnc dead(): int {
  return 99;
}
pub fnc exported_unused(): int {
  return 7;
}
"""

PRIVATE_ONLY = """fnc hidden(): int {
  return 1;
}
"""

OVERLOAD = """fnc pick(int x): int {
  return x + 1;
}
fnc pick(int x, int y): int {
  return x + y;
}
pub fnc main(): int {
  return pick(40, 2);
}
"""

DEAD_RUNTIME = """define class DeadBox as
  pub final int token: 99;
end

define actor DeadWorker as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}

fnc dead_array_runtime(): int {
  let mut Array<int> items = new Array<int>();
  items.add(1);
  return items.size;
}

pub fnc main(): int {
  return 42;
}
"""

LIVE_ACTOR = """define actor Worker as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
pub fnc main(): int {
  const worker: ActorRef<Worker> = spawn Worker();
  worker.send(1);
  stop worker;
  return 42;
}
"""

DEAD_OWNED = """fnc dead_owned_region(): int {
  const value: owned int = 7;
  const copied: int = value;
  return copied;
}
pub fnc main(): int {
  return 42;
}
"""

LIVE_OWNED = """pub fnc main(): int {
  const a: owned int = 6;
  val b: owned int = a * 7;
  const c: int = b;
  return c;
}
"""

LIVE_ARRAY = """pub fnc main(): int {
  let mut Array<int> items = new Array<int>();
  items.add(41);
  items.add(1);
  return items.get(0) + items.get(1);
}
"""

DEAD_WASM_INVALID = """fnc dead_wasm_invalid(): int {
  try {
    throw 7;
  } catch (err) {
    return err;
  } finally {
  }
}
pub fnc main(): int {
  return 42;
}
"""

RT_PRIVATE_ENTRY = """fnc control(int value): int {
  return value + 1;
}
pub fnc main(): int {
  return 42;
}
"""


def compile_ir(compiler, source, shaken, roots=(), extra=()):
    with tempfile.TemporaryDirectory(prefix="oreslang-tree-shake-source-") as td:
        path = Path(td) / "case.ores"
        path.write_text(source, encoding="utf-8")
        argv = [str(compiler), *extra]
        if shaken:
            argv.append("--tree-shake")
            argv.extend("--tree-shake-root=" + root for root in roots)
        argv.append(str(path))
        return run(argv)


def link_and_run(clang, ir_payload, expected, runtime_source=None):
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix="oreslang-tree-shake-link-") as td:
        directory = Path(td)
        ir = directory / "guest.ll"
        exe = directory / "guest"
        ir.write_bytes(ir_payload)
        command = [str(clang), "-O2", "-x", "ir", str(ir)]
        if runtime_source is not None:
            source = root / "src/main/cpp/oreslang/runtime" / runtime_source
            require(source.is_file(), "native runtime source unavailable: " + str(source))
            command.extend([
                "-x", "c", str(source), "-std=c11",
                "-I", str(root / "src/main/cpp")
            ])
        command.extend(["-o", str(exe)])
        linked = run(command)
        require(
            linked.returncode == 0,
            "clang link failed: " + repr(linked.stderr[:2048])
        )
        result = run([str(exe)])
        require(
            result.returncode == expected
            and result.stdout == b""
            and result.stderr == b"",
            "native behavior mismatch: rc=%d out=%r err=%r"
            % (result.returncode, result.stdout, result.stderr[:512])
        )
        return exe.stat().st_size


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    require(args.compiler.is_file(), "compiler unavailable")
    require(args.clang.is_file(), "clang unavailable")

    normal = compile_ir(args.compiler, SOURCE, False)
    conservative = compile_ir(args.compiler, SOURCE, True)
    shaken = compile_ir(args.compiler, SOURCE, True, ("main",))
    require(normal.returncode == 0, "unshaken compile failed: " + repr(normal.stderr[:1024]))
    require(conservative.returncode == 0, "conservative tree-shaken compile failed: " + repr(conservative.stderr[:1024]))
    require(shaken.returncode == 0, "exact-root tree-shaken compile failed: " + repr(shaken.stderr[:1024]))

    for symbol in (b"@live(", b"@dead(", b"@exported_unused("):
        require(symbol in normal.stdout, "unshaken IR missing expected symbol " + repr(symbol))
    require(b"@live(" in conservative.stdout, "reachable private helper was removed")
    require(b"@exported_unused(" in conservative.stdout, "conservative pub export root was removed")
    require(b"@dead(" not in conservative.stdout, "unreachable private function survived conservative tree shaking")
    require(b"@live(" in shaken.stdout, "reachable private helper was removed from exact-root build")
    require(b"@exported_unused(" not in shaken.stdout, "unused pub function survived exact executable roots")
    require(b"@dead(" not in shaken.stdout, "unreachable private function survived exact-root tree shaking")
    require(b"define i32 @main(" in shaken.stdout, "native entry wrapper missing")

    normal_bytes = link_and_run(args.clang, normal.stdout, 42)
    conservative_bytes = link_and_run(args.clang, conservative.stdout, 42)
    shaken_bytes = link_and_run(args.clang, shaken.stdout, 42)

    overloaded = compile_ir(args.compiler, OVERLOAD, True)
    require(overloaded.returncode == 0, "overload-aware tree shaking failed: " + repr(overloaded.stderr[:1024]))
    require(b"pick$arity2" in overloaded.stdout, "reachable arity-2 overload missing")
    require(b"pick$arity1" not in overloaded.stdout, "dead arity-1 overload survived")
    ambiguous = compile_ir(args.compiler, OVERLOAD, True, ("pick",))
    require(
        ambiguous.returncode != 0
        and ambiguous.stdout == b""
        and b"ambiguous tree-shake root" in ambiguous.stderr,
        "ambiguous explicit overload root did not fail closed"
    )

    runtime_normal = compile_ir(args.compiler, DEAD_RUNTIME, False)
    runtime_shaken = compile_ir(args.compiler, DEAD_RUNTIME, True, ("main",))
    require(runtime_normal.returncode == 0, "dead-runtime control compile failed")
    require(runtime_shaken.returncode == 0, "dead-runtime shaken compile failed: " + repr(runtime_shaken.stderr[:1024]))
    require(b"@dead_array_runtime" in runtime_normal.stdout, "control lost dead Array function")
    require(b"oreslang_int_array_try_region_open" in runtime_normal.stdout, "control lost Array runtime")
    require(b"oreslang_source_actor_spawn" in runtime_normal.stdout, "control lost actor runtime")
    for forbidden in (
        b"@dead_array_runtime",
        b"oreslang_int_array_try_region_open",
        b"oreslang_int_array_try_new",
        b"oreslang_int_array_region_close",
        b"oreslang_source_actor_spawn",
        b"ores_actor_i64_heap_field_read",
        b"DeadBox",
        b"DeadWorker",
    ):
        require(forbidden not in runtime_shaken.stdout,
                "dead runtime/type survived semantic pruning: " + repr(forbidden))
    link_and_run(args.clang, runtime_shaken.stdout, 42)

    live_actor = compile_ir(args.compiler, LIVE_ACTOR, True, ("main",))
    require(live_actor.returncode == 0, "live actor tree shaking failed: " + repr(live_actor.stderr[:1024]))
    require(b"oreslang_source_actor_spawn" in live_actor.stdout,
            "live actor runtime feature was incorrectly removed")
    require(b"oreslang_actor_Worker_on_message" in live_actor.stdout,
            "reachable actor handler was incorrectly removed")

    dead_owned_control = compile_ir(
        args.compiler, DEAD_OWNED, False, extra=("--experimental-owned-int",)
    )
    dead_owned_shaken = compile_ir(
        args.compiler, DEAD_OWNED, True, ("main",), ("--experimental-owned-int",)
    )
    require(dead_owned_control.returncode == 0, "owned-region control compile failed")
    require(dead_owned_shaken.returncode == 0, "owned-region shaken compile failed: " + repr(dead_owned_shaken.stderr[:1024]))
    require(b"oreslang_owned_i64_region_open" in dead_owned_control.stdout,
            "control lost owned-region runtime")
    for forbidden in (
        b"@dead_owned_region",
        b"oreslang_owned_i64_region_open",
        b"oreslang_owned_i64_region_new",
        b"oreslang_owned_i64_region_close",
    ):
        require(forbidden not in dead_owned_shaken.stdout,
                "dead owned runtime survived tree shaking: " + repr(forbidden))
    link_and_run(args.clang, dead_owned_shaken.stdout, 42)

    live_owned = compile_ir(
        args.compiler, LIVE_OWNED, True, ("main",), ("--experimental-owned-int",)
    )
    require(live_owned.returncode == 0, "live owned-runtime compile failed: " + repr(live_owned.stderr[:1024]))
    require(b"oreslang_owned_i64_region_open" in live_owned.stdout,
            "live owned-region feature was removed")
    link_and_run(args.clang, live_owned.stdout, 42, "NativeOwnedI64Region.c")

    live_array = compile_ir(args.compiler, LIVE_ARRAY, True, ("main",))
    require(live_array.returncode == 0, "live Array runtime compile failed: " + repr(live_array.stderr[:1024]))
    require(b"oreslang_int_array_try_region_open" in live_array.stdout,
            "live Array runtime feature was removed")
    require(b"oreslang_int_array_try_new" in live_array.stdout,
            "live Array allocation feature was removed")
    link_and_run(args.clang, live_array.stdout, 42, "NativeIntArrayRegion.c")

    rt_rooted = compile_ir(
        args.compiler,
        RT_PRIVATE_ENTRY,
        True,
        ("main",),
        ("--profile=native-hard-rt", "--rt-entry=control/1"),
    )
    require(
        rt_rooted.returncode == 0,
        "hard-RT + tree-shake compile failed: " + repr(rt_rooted.stderr[:1024])
    )
    require(
        b"@control(" in rt_rooted.stdout,
        "admitted private hard-RT entry was removed by tree shaking"
    )

    wasm_hidden = compile_ir(
        args.compiler,
        DEAD_WASM_INVALID,
        True,
        ("main",),
        ("--target=wasm32",),
    )
    require(
        wasm_hidden.returncode != 0
        and wasm_hidden.stdout == b""
        and b"wasm32 native caught-throw guest unsupported" in wasm_hidden.stderr,
        "tree shaking hid dead target-invalid Wasm source"
    )

    rejected = compile_ir(args.compiler, PRIVATE_ONLY, True)
    require(
        rejected.returncode != 0
        and rejected.stdout == b""
        and b"requires an explicit root, main, or at least one pub function" in rejected.stderr,
        "rootless strict tree shaking did not fail closed"
    )
    missing = compile_ir(args.compiler, SOURCE, True, ("missing",))
    require(
        missing.returncode != 0
        and missing.stdout == b""
        and b"unknown tree-shake root" in missing.stderr,
        "unknown explicit tree-shake root did not fail closed"
    )

    print(
        "TREE_SHAKE_RUNTIME_FEATURES_OK "
        "normal_bytes=%d conservative_bytes=%d shaken_bytes=%d"
        % (normal_bytes, conservative_bytes, shaken_bytes)
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("LLVM TREE SHAKING DENIED: " + str(error), file=sys.stderr)
        raise SystemExit(1)
