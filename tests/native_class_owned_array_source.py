#!/usr/bin/env python3
"""Real Oreslang-source -> LLVM -> Clang class-owned Array<int> admission.

The C ABI aborts on failure. This slice establishes no partial object
publication and exact-once normal/guarded-return cleanup, not recovery from
allocation failure, guest exception unwinding or actor thread safety.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

CLASS = """define class Shelf as
  pub let int number = 7;
  let mut Array<int> first = new Array<int>();
  let mut Array<int> second = new Array<int>();
end
"""
POSITIVES = {
    "class-array-whole-move": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add(19);
  let mut Shelf moved = rt take(shelf);
  moved.second.add(23);
  return moved.first.get(0) + moved.second.get(0);
}
""", 42, 2),
    "class-array-chain-moves": (CLASS + """pub fnc main(): int {
  let mut Shelf original = new Shelf();
  original.first.add(6);
  let mut Shelf middle = rt take(original);
  let mut Shelf final_owner = rt take(middle);
  final_owner.second.add(36);
  return final_owner.first.get(0) + final_owner.second.get(0);
}
""", 42, 2),
    "class-array-move-followed-by-allocation": (CLASS + """pub fnc main(): int {
  let mut Shelf original = new Shelf();
  original.first.add(10);
  let mut Shelf relocated = rt take(original);
  let mut Shelf new_one = new Shelf();
  new_one.first.add(11);
  relocated.second.add(21);
  return relocated.first.get(0) + relocated.second.get(0) + new_one.first.get(0);
}
""", 42, 4),
    "class-array-move-guarded-exit": (CLASS + """pub fnc main(): int {
  let mut Shelf original = new Shelf();
  original.first.add(42);
  let mut Shelf relocated = rt take(original);
  if (relocated.first.size == 1) { return relocated.first.get(0); } fi
  return 2;
}
""", 42, 2),
    # Exact admitted boundary: four independently constructed classes, each
    # owning sixteen arrays. This tests all 64 handles and reverse-order drop.
    "exact-64-owned-array-limit": ("""define class Slot16 as
""" + "".join(f"  let mut Array<int> f{i} = new Array<int>();\n"
             for i in range(16)) + """end
pub fnc main(): int {
  let mut Slot16 a = new Slot16();
  let mut Slot16 b = new Slot16();
  let mut Slot16 c = new Slot16();
  let mut Slot16 d = new Slot16();
  d.f15.add(42);
  return d.f15.get(0);
}
""", 42, 64),
    "class-constructor-effect-alloc": (CLASS + """pub fnc main(): int using alloc {
  let mut Shelf shelf = new Shelf();
  return shelf.number + 35;
}
""", 42, 2),
    "direct-scalar-field-write": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.number = 42;
  return shelf.number;
}
""", 42, 2),
    "two-fields": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add(19);
  shelf.second.add(23);
  return shelf.first.get(0) + shelf.second.get(0);
}
""", 42, 2),
    "mixed-array-and-class": (CLASS + """pub fnc main(): int {
  let mut Array<int> local = new Array<int>();
  let mut Shelf shelf = new Shelf();
  local.add(5);
  shelf.first.add(10);
  shelf.second.add(27);
  return local.get(0) + shelf.first.get(0) + shelf.second.get(0);
}
""", 42, 3),
    "two-independent-classes": (CLASS + """pub fnc main(): int {
  let mut Shelf first = new Shelf();
  let mut Shelf other = new Shelf();
  first.first.add(7);
  other.first.add(35);
  return first.first.get(0) + other.first.get(0);
}
""", 42, 4),
    "size-and-scalar": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add(20);
  shelf.first.add(1);
  shelf.first.set(1, 15);
  return shelf.first.get(0) + shelf.first.get(1) + shelf.first.size + shelf.number + -2;
}
""", 42, 2),
    "early-return-drop": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add(42);
  if (shelf.first.size == 1) { return shelf.first.get(0); } fi
  return 1;
}
""", 42, 2),
    "guard-fallthrough-drop": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add(42);
  if (shelf.first.size == 0) { return 3; } fi
  return shelf.first.get(0);
}
""", 42, 2),
    "conditional-mutation": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  if (shelf.number == 7) {
    shelf.first.add(21);
  } else {
    shelf.first.add(3);
  } fi
  shelf.second.add(2);
  return shelf.first.get(0) * shelf.second.get(0);
}
""", 42, 2),
}
NEGATIVES = {
    "class-allocation-must-declare-alloc": (CLASS + """pub fnc main(): int using state.read {
  let mut Shelf shelf = new Shelf();
  return shelf.number;
}
""", "owned allocation violates using signature without alloc"),
    # Before this guard, nested writes could silently target an unrelated
    # local object sharing the nested receiver's final field name.
    "nested-write-colliding-local": ("""define class Bucket as
  let mut Array<int> items = new Array<int>();
end
define class Counter as
  pub let int size = 1;
end
pub fnc main(): int {
  let mut Bucket bucket = new Bucket();
  let mut Counter items = new Counter();
  bucket.items.size = 99;
  return items.size;
}
""", "native field assignment requires direct named class receiver"),
    "nested-write-no-collision": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.size = 42;
  return shelf.first.size;
}
""", "native field assignment requires direct named class receiver"),
    "nested-actor-send-colliding-local": ("""define class Box as
  pub let int items = 0;
end
define actor Worker as {
  let total: int = 0;
  pub on_message(int n): int { return total + n; }
}
pub fnc main(): int {
  const items = spawn Worker();
  let mut Box box = new Box();
  box.items.send(5);
  stop items;
  return 42;
}
""", "native actor send requires direct named ActorRef receiver"),
    "nested-take-array-size": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return rt take(shelf.first.size);
}
""", "native rt take requires a direct named class field"),
    "immutable-owner": (CLASS + """pub fnc main(): int {
  let Shelf shelf = new Shelf();
  return shelf.number;
}
""", "class with Array<int> fields requires let mut owner"),
    "forbid-reuse-after-move": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  let mut Shelf moved = rt take(shelf);
  return shelf.first.size;
}
""", "use-after-move"),
    "forbid-duplicate-class-move": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  let mut Shelf first = rt take(shelf);
  let mut Shelf second = rt take(shelf);
  return first.number;
}
""", "class move requires live unborrowed owner"),
    "forbid-immutable-move-target": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  let Shelf moved = rt take(shelf);
  return moved.number;
}
""", "moved Array<int> class requires let mut owner"),
    "forbid-borrowed-parameter": (CLASS + """fnc read(borrow Shelf shelf): int {
  return shelf.number;
}
pub fnc main(): int {
  return 42;
}
""", "Array<int> class cannot be borrowed or passed"),
    "forbid-parameter-mut": (CLASS + """fnc change(borrow mut Shelf shelf): int {
  return shelf.number;
}
pub fnc main(): int { return 42; }
""", "Array<int> class cannot be borrowed or passed"),
    "forbid-field-value-escape": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  const leaked: int = shelf.first;
  return leaked;
}
""", "Array<int> field is non-Copy and cannot escape"),
    "forbid-return-field": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return shelf.first;
}
""", "Array<int> field is non-Copy and cannot escape"),
    "forbid-reassign-handle": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first = 12;
  return 0;
}
""", "cannot reassign owned Array<int> class field"),
    "forbid-deferred-reassign": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  defer shelf.first = 12;
  return 0;
}
""", "cannot reassign owned Array<int> class field"),
    "forbid-take-field": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return rt take(shelf.first);
}
""", "Array<int> field is non-Copy and cannot escape"),
    "forbid-fake-array-receiver": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.number.add(4);
  return 42;
}
""", "requires an owned Array<int> place"),
    "forbid-size-on-scalar": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return shelf.number.size;
}
""", "requires an owned Array<int> place"),
    "forbid-unknown-field": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.missing.add(5);
  return 42;
}
""", "requires an owned Array<int> place"),
    "forbid-method-shape": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return shelf.first.set(0, 3);
}
""", "expression supports only get(index)"),
    "forbid-method-arity": (CLASS + """pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.first.add();
  return 42;
}
""", "effect supports only add(value) or set(index,value)"),
    "forbid-handle-passing": (CLASS + """fnc id(int x): int { return x; }
pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return id(shelf.second);
}
""", "Array<int> field is non-Copy and cannot escape"),
    "forbid-false-constructor": ("""define class Shelf as
  let mut Array<int> items = new Array<int>(5);
end
pub fnc main(): int { return 42; }
""", "expected ')'"),
    "forbid-untyped-array": ("""define class Shelf as
  let Array<int> items = new Array<int>();
end
pub fnc main(): int { return 42; }
""", "expected 'int'"),
    "exceed-64-array-quota": ("""define class Shelf as
""" + "".join(f"  let mut Array<int> item{i} = new Array<int>();\n"
               for i in range(8)) + """end
pub fnc main(): int {
""" + "".join(f"  let mut Shelf shelf{i} = new Shelf();\n"
               for i in range(9)) + """  return 42;
}
""", "per-function owned handle count exceeds 64"),
}

def run(argv):
    return subprocess.run(argv, capture_output=True, timeout=90, check=False)

def check(compiler, clang, source_root, sanitizers):
    runtime = source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = source_root / "src/main/cpp"
    for path in (compiler, clang, runtime):
        if not path.is_file():
            raise AssertionError(f"missing gate input: {path}")
    with tempfile.TemporaryDirectory(prefix="ores-native-owned-class-array-") as temp:
        work = Path(temp)
        for name, (src, expected, count) in POSITIVES.items():
            source = work / (name + ".ores")
            source.write_text(src, encoding="utf-8")
            compiled = run([str(compiler), str(source)])
            if compiled.returncode or compiled.stderr or not compiled.stdout:
                raise AssertionError(f"{name}: compiler failed: {compiled.stderr!r}")
            ir = compiled.stdout
            if ir.count(b"call i32 @oreslang_int_array_try_new(") != count or \
                    ir.count(b"call void @oreslang_int_array_drop(") < count or \
                    b"@oreslang_int_array_region_close" not in ir or \
                    b"@oreslang_int_array_try_region_open" not in ir or \
                    b"array.resource.failure:" not in ir:
                raise AssertionError(f"{name}: missing compiler-generated owned ABI operations")
            if name == "two-fields":
                a = ir.find(b"shelf.second.drop.handle")
                b = ir.find(b"shelf.first.drop.handle")
                if a < 0 or b < 0 or a >= b:
                    raise AssertionError("owned field destructor order must reverse declarations")
            ll = work / (name + ".ll")
            ll.write_bytes(ir)
            exe = work / name
            command = [str(clang), "-O1", "-Wno-override-module",
                       "-std=c11", "-x", "ir", str(ll),
                       "-x", "c", str(runtime), "-I", str(include),
                       "-o", str(exe)]
            if sanitizers:
                command[1:1] = ["-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer"]
            linked = run(command)
            if linked.returncode:
                raise AssertionError(f"{name}: clang/link failure {linked.stderr!r}")
            result = run([str(exe)])
            if result.returncode != expected or result.stdout or result.stderr:
                raise AssertionError(f"{name}: incorrect/sanitizer-failed execution: "
                                     f"{result.returncode}, {result.stdout!r}, {result.stderr!r}")
        # The nested-send rejection must not break a legitimate direct
        # ActorRef.send(...) call. This is a compiler/IR control: the isolated
        # test runner does not link the actor scheduler runtime.
        actor_source = work / "direct-actor-send.ores"
        actor_source.write_text("""define actor Worker as {
  let total: int = 0;
  pub on_message(int n): int { return total + n; }
}
pub fnc main(): int {
  const worker = spawn Worker();
  worker.send(1);
  stop worker;
  return 42;
}
""", encoding="utf-8")
        actor_ir = run([str(compiler), str(actor_source)])
        if actor_ir.returncode or actor_ir.stderr or \
                b"@oreslang_source_actor_send" not in actor_ir.stdout or \
                b"@oreslang_actor_Worker_on_message" not in actor_ir.stdout:
            raise AssertionError("valid direct actor send must still compile to actor ABI IR")
        for name, (src, diagnostic) in NEGATIVES.items():
            source = work / (name + ".ores")
            source.write_text(src, encoding="utf-8")
            rejected = run([str(compiler), str(source)])
            if rejected.returncode != 1 or rejected.stdout or \
                    diagnostic.encode() not in rejected.stderr:
                raise AssertionError(f"{name}: not fail-closed with {diagnostic}: "
                                     f"{rejected.returncode}, {rejected.stdout!r}, {rejected.stderr!r}")
        wasm = work / "wasm-class.ores"
        wasm.write_text(POSITIVES["two-fields"][0], encoding="utf-8")
        rejected = run([str(compiler), "--target=wasm32", str(wasm)])
        if rejected.returncode != 1 or rejected.stdout or \
                b"wasm32 class Array<int> owned fields are not implemented" not in rejected.stderr:
            raise AssertionError("wasm32 owned class must fail closed with no IR")
    print(f"PASS: class-owned Array<int> source: {len(POSITIVES)} native guests, "
          f"{len(NEGATIVES)} ownership rejects, wasm32 no-IR reject"
          + (" (ASan+UBSan)" if sanitizers else ""), flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--sanitizers", action="store_true")
    opts = parser.parse_args()
    try:
        check(opts.compiler.resolve(), opts.clang.resolve(),
              opts.source_root.resolve(), opts.sanitizers)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"native class-owned array gate FAILED: {error}", file=sys.stderr)
        sys.exit(1)
