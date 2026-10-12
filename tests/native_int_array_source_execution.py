#!/usr/bin/env python3
"""End-to-end native Oreslang Array<int> source lowering admission.

Exercises *compiler generated* LLVM IR, NOT the handwritten ABI fixture.
A negative case must fail before printing any IR.
"""
import argparse
from pathlib import Path
import signal
import subprocess
import sys
import tempfile


POSITIVE = {
    "add-get-set-size": ("""pub fnc main(): int {
  let mut Array<int> numbers = new Array<int>();
  numbers.add(4);
  numbers.add(7);
  numbers.set(0, 35);
  return numbers.get(0) + numbers.get(1) + numbers.size;
}
""", 44, 1),
    "two-owner-reverse-drop": ("""pub fnc main(): int {
  let mut Array<int> first = new Array<int>();
  let mut Array<int> second = new Array<int>();
  first.add(18);
  second.add(23);
  first.set(0, first.get(0) + 1);
  return first.get(0) + second.get(0);
}
""", 42, 2),
    "early-return-drop": ("""pub fnc main(): int {
  let mut Array<int> first = new Array<int>();
  first.add(99);
  if (first.size == 1) { return 42; } fi
  return 2;
}
""", 42, 1),
    "fallthrough-drop": ("""pub fnc main(): int {
  let mut Array<int> first = new Array<int>();
  first.add(12);
  if (first.size == 0) { return 3; } fi
  return first.get(0) + 30;
}
""", 42, 1),
    "conditional-arm-mutation": ("""pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  if (a.size == 0) {
    a.add(21);
  } else {
    a.add(99);
  } fi
  a.set(0, a.get(0) * 2);
  return a.get(0);
}
""", 42, 1),
    "argument-evaluation": ("""fnc inc(int x): int { return x + 1; }
pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  a.add(inc(40));
  a.set(0, inc(a.get(0)));
  return a.get(0);
}
""", 42, 1),
}

NEGATIVE = {
    "reserved-abi-symbol": ("""fnc oreslang_int_array_get(int x): int {
  return x;
}
pub fnc main(): int { return 42; }
""", "reserved native Array<int> ABI symbol"),
    "too-many-owners": ("pub fnc main(): int {\n" +
        "".join(f"  let mut Array<int> a{i} = new Array<int>();\n" for i in range(65)) +
        "  return 0;\n}\n", "native Array<int> owner limit exceeded (64)"),
    "scalar-call-escape": ("""fnc id(int x): int { return x; }
pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  return id(a);
}
""", "Array<int> is non-Copy"),
    "implicit-array-copy": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 const leaked: int = a;
 return 0;
}
""", "Array<int> is non-Copy"),
    "return-handle": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 return a;
}
""", "Array<int> is non-Copy"),
    "array-move": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 const stolen: int = rt take(a);
 return stolen;
}
""", "Array<int> is non-Copy"),
    "array-loan": ("""fnc read(borrow int x): int { return x; }
pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 return read(rt borrow a);
}
""", "Array<int> cannot be borrowed"),
    "immutable-array": ("""pub fnc main(): int {
 let Array<int> a = new Array<int>();
 return a.size;
}
""", "Array<int> requires let mut owner"),
    "unsupported-method": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 a.remove(1);
 return 0;
}
""", "supports only add(value) or set(index,value)"),
    "bad-receiver": ("""pub fnc main(): int {
 const a: int = 5;
 a.add(1);
 return 0;
}
""", "requires live let mut owner"),
    "get-arity": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 a.add(1);
 return a.get(0, 1);
}
""", "supports only get(index)"),
    "unsupported-field": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 return a.capacity;
}
""", "only Array<int>.size is readable"),
    "get-as-effect": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 a.get(1);
 return 0;
}
""", "effect supports only add(value) or set(index,value)"),
    "add-as-value": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 return a.add(2);
}
""", "expression supports only get(index)"),
    "bad-set-arity": ("""pub fnc main(): int {
 let mut Array<int> a = new Array<int>();
 a.set(0);
 return 0;
}
""", "effect supports only add(value) or set(index,value)"),
}


def run(command, *, input=None):
    return subprocess.run(command, input=input, capture_output=True, timeout=80, check=False)


def verify(compiler: Path, clang: Path, root: Path, sanitized: bool):
    c_runtime = root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = root / "src/main/cpp"
    for path in (compiler, clang, c_runtime):
        if not path.is_file():
            raise AssertionError(f"missing native source gate input: {path}")
    with tempfile.TemporaryDirectory(prefix="oreslang-array-source-") as directory:
        work = Path(directory)
        for name, (source, exit_code, owners) in POSITIVE.items():
            source_path = work / f"{name}.ores"
            source_path.write_text(source, encoding="utf-8")
            compiled = run([str(compiler), str(source_path)])
            if compiled.returncode != 0 or compiled.stderr:
                raise AssertionError(f"{name} compiler rc={compiled.returncode}: {compiled.stderr!r}")
            ir = compiled.stdout
            for required in (b"; ModuleID", b"@oreslang_int_array_region_open",
                             b"@oreslang_int_array_new", b"@oreslang_int_array_drop",
                             b"@oreslang_int_array_region_close"):
                if required not in ir:
                    raise AssertionError(f"{name}: compiler did not emit {required!r}")
            if ir.count(b"call i64 @oreslang_int_array_new(") != owners:
                raise AssertionError(f"{name}: did not allocate exact owned array count")
            if not ir or b"define i32 @main()" not in ir:
                raise AssertionError(f"{name}: missing native compiler-generated entrypoint")
            ll = work / f"{name}.ll"
            ll.write_bytes(ir)
            exe = work / name
            link = [str(clang), "-O1", "-Wno-override-module", "-std=c11",
                    "-x", "ir", str(ll), "-x", "c", str(c_runtime),
                    "-I", str(include), "-o", str(exe)]
            if sanitized:
                link[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
            linked = run(link)
            if linked.returncode != 0:
                raise AssertionError(f"{name}: link rc={linked.returncode}: {linked.stderr!r}")
            result = run([str(exe)])
            if result.returncode != exit_code or result.stdout or result.stderr:
                raise AssertionError(
                    f"{name}: native exit={result.returncode}, expected={exit_code}, "
                    f"stdout={result.stdout!r}, stderr={result.stderr!r}")
        # No arrays are allowed in wasm32 until a bounded guest runtime and
        # capability ABI are independently implemented; rejection emits no IR.
        wasm_src = work / "wasm-array.ores"
        wasm_src.write_text(POSITIVE["add-get-set-size"][0], encoding="utf-8")
        wasm = run([str(compiler), "--target=wasm32", str(wasm_src)])
        if wasm.returncode != 1 or wasm.stdout or \
                b"wasm32 native heap-owned/Array<int> regions are not implemented" not in wasm.stderr:
            raise AssertionError(f"wasm32 array must reject without IR: {wasm.returncode}, {wasm.stderr!r}")
        for name, (source, diagnostic) in NEGATIVE.items():
            src = work / f"{name}.ores"
            src.write_text(source, encoding="utf-8")
            rejected = run([str(compiler), str(src)])
            if rejected.returncode != 1 or rejected.stdout or \
                    not rejected.stderr.startswith(b"oreslang-llvmc: ") or \
                    diagnostic.encode() not in rejected.stderr:
                raise AssertionError(
                    f"{name}: expected fail-closed '{diagnostic}', "
                    f"rc={rejected.returncode}, stdout={rejected.stdout!r}, "
                    f"stderr={rejected.stderr!r}")
    print(f"PASS native source Array<int>: {len(POSITIVE)} executables, "
          f"{len(NEGATIVE)} no-IR ownership gates"
          + (" (ASan+UBSan)" if sanitized else ""), flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.compiler.resolve(), args.clang.resolve(),
               args.source_root.resolve(), args.sanitizers)
    except (AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Native Array<int> source gate FAILED: {error}", file=sys.stderr)
        sys.exit(1)
