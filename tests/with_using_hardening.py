#!/usr/bin/env python3
"""Adversarial source/ABI regressions for canonical actor state scopes."""
import argparse
import pathlib
import shutil
import signal
import subprocess
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, timeout=60, **kwargs)


def require(ok, detail):
    if not ok:
        raise AssertionError(detail)


def checked(command, **kwargs):
    result = run(command, **kwargs)
    require(result.returncode == 0, (command, result.returncode, result.stdout, result.stderr))
    return result.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", required=True)
    parser.add_argument("--cxx", default=shutil.which("clang++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    compiler = str(pathlib.Path(args.compiler).resolve())
    root = pathlib.Path(args.root).resolve()
    with tempfile.TemporaryDirectory(prefix="with-using-hardening-") as folder:
        temp = pathlib.Path(folder)

        def compile_source(name, source, flags=()):
            path = temp / (name + ".ores")
            path.write_text(source)
            return run([compiler, *flags, str(path)])

        def reject(name, source, diagnostic, flags=()):
            result = compile_source(name, source, flags)
            require(result.returncode == 1 and not result.stdout.strip(),
                    (name, "must reject without partial IR or a compiler crash", result))
            require(diagnostic in result.stderr, (name, result.stderr))

        def actor(name, member):
            return f"define actor {name} as state {{ x: int = 0; }} " + member + " end\n"

        def read_method(name):
            return f"{name}(): int with state.read {{ return using state.read(|s| -> s.x); }}"

        for index, (left, right) in enumerate([
            (actor("A_B", read_method("C")), actor("A", read_method("B_C"))),
            (actor("A", read_method("B_init")), actor("A_B", read_method("read"))),
            (actor("A_B", read_method("read")), actor("A", read_method("B_init"))),
        ]):
            reject(f"symbol-collision-{index}", left + right, "conflicting generated actor symbol")
        distinct = compile_source("distinct-symbols", actor("A_B", read_method("C"))
                                  + actor("A", read_method("B_D")))
        require(distinct.returncode == 0, distinct.stderr)
        require("@oreslang_actor_A_B_C(" in distinct.stdout
                and "@oreslang_actor_A_B_D(" in distinct.stdout, distinct.stdout)
        print("PASS actor/method and constructor ABI collision fences", flush=True)

        def read_expression(expr):
            return actor("Counter", "run(): int with state.read { return using state.read(|s| -> " + expr + "); }")

        # Inputs are far below the file-size limit. They previously crashed
        # the compiler rather than producing a language admission diagnostic.
        for label, expr, diagnostic in (
            ("deep-groups", "(" * 100000 + "s.x" + ")" * 100000, "state callback parser nesting limit"),
            ("deep-sum", "+".join(["s.x"] * 100000), "state callback token budget"),
            ("deep-unary", "-" * 100000 + "s.x", "state callback parser nesting limit"),
            ("ast-depth", "+".join(["s.x"] * 67), "state callback expression depth limit"),
        ):
            reject(label, read_expression(expr), diagnostic)
        # Boundary checks keep ordinary callbacks usable and deterministic.
        for label, expr in (("groups-boundary", "(" * 63 + "s.x" + ")" * 63),
                            ("sum-boundary", "+".join(["s.x"] * 65))):
            accepted = compile_source(label, read_expression(expr))
            require(accepted.returncode == 0, (label, accepted.stderr))
        reject("groups-over-boundary", read_expression("(" * 64 + "s.x" + ")" * 64),
               "state callback parser nesting limit")
        reject("sum-over-boundary", read_expression("+".join(["s.x"] * 66)),
               "state callback expression depth limit")
        print("PASS bounded callback parsing, AST effects and resource boundary cases", flush=True)
        if args.sanitize:
            require(args.cxx, "sanitized parser checks require a C++ compiler")
            parser_test = temp / "parser-budget"
            checked([args.cxx, "-std=c++17", "-O1", "-fsanitize=address,undefined",
                     "-fno-omit-frame-pointer", "-I" + str(root / "src/main/cpp"),
                     str(root / "tests/state_callback_budget_test.cpp"),
                     str(root / "src/main/cpp/oreslang/frontend/Parser.cpp"),
                     str(root / "src/main/cpp/oreslang/types/OwnershipChecker.cpp"),
                     "-o", str(parser_test)])
            checked([str(parser_test)], cwd=temp)
            print("PASS production parser/ownership sources under ASan/UBSan", flush=True)

        bootstrap = "pub fnc main(): int { const a = spawn Counter(); a.send(1); stop a; return 43; }"
        entry = "on_message(int n): int with state.read { return using state.read(|s| -> s.x); }"
        for extra in ("", "unchecked(): int { return main(); }"):
            source = actor("Counter", entry + extra) + bootstrap
            reject("cortex-unchecked-method", source, "cortex-m scoped state methods",
                   ("--target=thumbv7m-none-eabi",))
        native = compile_source("native-supported", actor("Counter", entry))
        wasm = compile_source("wasm-supported", actor("Counter", entry), ("--target=wasm32",))
        require(native.returncode == 0 and wasm.returncode == 0, (native.stderr, wasm.stderr))
        print("PASS Cortex-M target admission; native/Wasm scopes remain admitted", flush=True)

        source = actor("Counter", '''
          read_noop(): int with state.read { return using state.read(|s| -> 7); }
          write_noop(): int with state.write { return using state.write(critical |s| -> 8); }
          constant_value(): int { return 9; }
        ''')
        result = compile_source("entry-fence", source)
        require(result.returncode == 0, result.stderr)
        require(result.stdout.count("call void @ores_actor_i64_heap_validate_turn(") == 3,
                "every canonical method must validate its entry, including no-op scopes")
        require("call i64 @ores_actor_i64_heap_field_read(" not in result.stdout,
                "entry validation must not manufacture state reads")
        ir = temp / "entry-fence.ll"
        ir.write_text(result.stdout)
        harness = temp / "entry-fence.c"
        harness.write_text('''#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
extern void oreslang_actor_Counter_init(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_read_noop(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_write_noop(OresActorI64Heap*,uint64_t,uint64_t);
extern int64_t oreslang_actor_Counter_constant_value(OresActorI64Heap*,uint64_t,uint64_t);
typedef int64_t (*Method)(OresActorI64Heap*,uint64_t,uint64_t);
int main(int argc, char** argv) {
  const struct rlimit no_core = {0,0};
  if (setrlimit(RLIMIT_CORE,&no_core) != 0 || argc != 3) return 90;
  const Method methods[] = {oreslang_actor_Counter_read_noop,
      oreslang_actor_Counter_write_noop, oreslang_actor_Counter_constant_value};
  const int method = atoi(argv[1]);
  if (method < 0 || method > 2) return 91;
  OresActorI64Heap* heap = ores_actor_i64_heap_open(123,7,1);
  OresActorTurn init = ores_actor_i64_heap_enter(heap,123,7);
  oreslang_actor_Counter_init(heap,init.domain,init.epoch);
  ores_actor_i64_heap_leave(heap,init);
  OresActorTurn turn = ores_actor_i64_heap_enter(heap,123,7);
  const char* mode = argv[2];
  if (!strcmp(mode,"null")) heap = NULL;
  else if (!strcmp(mode,"inactive")) ores_actor_i64_heap_leave(heap,turn);
  else if (!strcmp(mode,"stale")) {
    ores_actor_i64_heap_leave(heap,turn);
    (void) ores_actor_i64_heap_enter(heap,123,7);
  } else if (!strcmp(mode,"retired")) ores_actor_i64_heap_retire(heap);
  else if (!strcmp(mode,"foreign")) {
    OresActorI64Heap* other = ores_actor_i64_heap_open(456,7,1);
    turn = ores_actor_i64_heap_enter(other,456,7);
  } else if (strcmp(mode,"valid")) return 92;
  const int64_t result = methods[method](heap,turn.domain,turn.epoch);
  if (strcmp(mode,"valid")) return 93; /* Invalid entry must abort before this. */
  ores_actor_i64_heap_leave(heap,turn);
  ores_actor_i64_heap_retire(heap);
  return result == 7 + method && ores_actor_i64_heap_try_destroy(heap) == 1 ? 0 : 94;
}
''')
        binary = temp / "entry-fence"
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"] if args.sanitize else []
        checked([args.clang, "-O2", *sanitizer, str(ir), str(harness),
                 str(root / "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"),
                 "-I" + str(root / "src/main/cpp"), "-o", str(binary)])
        for method in range(3):
            for mode in ("valid", "null", "inactive", "stale", "retired", "foreign"):
                executed = run([str(binary), str(method), mode], cwd=temp)
                expected = 0 if mode == "valid" else -signal.SIGABRT
                require(executed.returncode == expected,
                        (method, mode, executed.returncode, executed.stderr))
        print("PASS 18 compiled entry checks: live, null, inactive, stale, retired, foreign", flush=True)


if __name__ == "__main__":
    main()
