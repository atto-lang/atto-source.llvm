#!/usr/bin/env python3
"""Deterministic native LLVM ownership/CFG regression corpus, independently run.

This deliberately exercises the actual oreslang-llvmc -> LLVM IR -> Clang native
code pipeline. It tests the admitted scalar subset, not full Rust-type safety.
"""
import argparse
import pathlib
import random
import subprocess
import tempfile


def run(command, *, input_text=None, timeout=25):
    return subprocess.run(command, input=input_text, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False, timeout=timeout)


def check(ok, message):
    if not ok:
        raise AssertionError(message)


def compiler_ir(compiler, path, experimental=False, accepted=True):
    command = [compiler]
    if experimental:
        command.append("--experimental-owned-int")
    command.append(str(path))
    result = run(command)
    if accepted:
        check(result.returncode == 0 and not result.stderr,
              f"compiler unexpectedly rejected {path}: {result.stderr}")
        check(result.stdout.startswith("; ModuleID"),
              f"compiler returned invalid IR for {path}")
        return result.stdout
    check(result.returncode != 0 and not result.stdout.strip(),
          f"invalid input emitted IR or succeeded: {path}: {result.stdout[:500]}")
    return result.stderr


def native_exit(ir, clang, region_source, use_region):
    with tempfile.TemporaryDirectory(prefix="ores-owned-native-") as directory:
        binary = str(pathlib.Path(directory) / "guest")
        command = [clang, "-x", "ir", "-"]
        if use_region:
            native = pathlib.Path(region_source)
            command.extend(["-x", "c", str(native), "-std=c11",
                            "-I", str(native.parents[2])])
        command.extend(["-o", binary])
        built = run(command, input_text=ir, timeout=60)
        check(built.returncode == 0,
              f"Clang native build rejected verified IR: {built.stderr}")
        result = run([binary], timeout=10)
        check(not result.stderr and result.returncode >= 0,
              f"native program failed: {result.returncode}: {result.stderr}")
        return result.returncode


def fixture(names, values, guards, selected, *, experimental=False):
    ownership = "owned int" if experimental else "int"
    declarations = [f"  const {name}: {ownership} = {val};"
                    for name, val in zip(names, values)]
    expected = 20 + selected if selected < guards else 42
    if_exprs = []
    for i in range(guards):
        name = names[i % len(names)]
        target = values[i % len(values)] if i == selected else 999
        operator = "eq" if i % 2 == 0 else "=="
        if_exprs.extend([f"  if ({name} {operator} {target}) {{",
                         f"    return {20 + i};", "  } fi"])
    source = "\n".join(["pub fnc main(): int {", *declarations,
                         *if_exprs, "  return 42;", "}", ""])
    return source, expected, len(declarations)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--clang", required=True)
    ap.add_argument("--native-runtime", required=True)
    args = ap.parse_args()
    rng = random.Random(0x0E521)
    total = 0
    with tempfile.TemporaryDirectory(prefix="ores-owned-corpus-") as root:
        root = pathlib.Path(root)

        # Standard by-value ints must stay heap/GC-free, even when a
        # variable, parameter, or function is literally named 'owned'.
        special = [
            """pub fnc main(): int {
  const owned: int = 40;
  return owned + 2;
}
""",
            """pub fnc owned(int owned): int {
  return owned + 1;
}
pub fnc main(): int {
  return owned(41);
}
""",
            """pub fnc main(): int {
  const owned: int = 41;
  if (owned eq 41) { return 42; } fi
  return 0;
}
"""
        ]
        for i, source in enumerate(special):
            source_file = root / f"ordinary-identifier-{i}.ores"
            source_file.write_text(source)
            ir = compiler_ir(args.compiler, source_file)
            check("oreslang_owned_i64_region_" not in ir,
                  "ordinary source declared a native heap ABI")
            check("gc.statepoint" not in ir and "gc.root" not in ir,
                  "ordinary source added a GC intrinsic")
            check(native_exit(ir, args.clang, args.native_runtime, False) == 42,
                  "ordinary source returned an unexpected result")
            total += 1

        # Deterministic input variation across the conditional early-return
        # CFG, both admitted comparison spellings, and 1..8 scalar locals.
        for experimental in (False, True):
            for i in range(32):
                n = 1 + rng.randrange(8)
                guards = rng.randrange(5)
                selected = rng.randrange(guards + 1)
                names = [f"v{j}" for j in range(n)]
                values = [1 + rng.randrange(80) for _ in range(n)]
                source, expected, owned_count = fixture(
                    names, values, guards, selected, experimental=experimental)
                path = root / f"{'heap' if experimental else 'stack'}-{i}.ores"
                path.write_text(source)
                ir = compiler_ir(args.compiler, path, experimental)
                check("gc.statepoint" not in ir and "gc.root" not in ir,
                      f"GC intrinsic in {path}")
                if experimental:
                    check(ir.count("call ptr @oreslang_owned_i64_region_new")
                          == owned_count,
                          f"native owners mismatch in {path}")
                    check(ir.count("call void @oreslang_owned_i64_region_drop")
                          == owned_count * (guards + 1),
                          f"not all return paths drop every owner: {path}")
                    check(ir.count("call void @oreslang_owned_i64_region_close")
                          == guards + 1,
                          f"not all returns close region: {path}")
                else:
                    check("oreslang_owned_i64_region_" not in ir,
                          f"ordinary int path declares heap ABI: {path}")
                actual = native_exit(ir, args.clang, args.native_runtime, experimental)
                check(actual == expected, f"{path}: native {actual}, expected {expected}")
                total += 1

        # No public 'owned' type: default must reject all such declarations.
        # The explicit experiment is narrower: primitive int only.
        negative = [
            "  const x: owned int = 42;",
            "  const owned: owned int = 42;",
            "  const x: owned Widget = 42;",
            "  const x: owned int = 42;\n  const y: owned int = x;",
        ]
        for i, local in enumerate(negative):
            path = root / f"reject-default-{i}.ores"
            path.write_text(f"pub fnc main(): int {{\n{local}\n  return 42;\n}}\n")
            diagnostic = compiler_ir(args.compiler, path, accepted=False)
            if "owned int" in local:
                check("experimental LLVM-only syntax" in diagnostic,
                      f"missing opt-in diagnostic: {diagnostic}")
            total += 1

        # Guard-local construction, missing fallthrough and untyped
        # conditions must reject even in the experimental compiler.
        invalid = [
            """pub fnc main(): int {
  const x: owned int = 4;
  if (x eq 4) { const y: owned int = 7; return y; } fi
  return x;
}
""",
            """pub fnc main(): int {
  const x: owned int = 4;
  if (x eq 4) { return x; } fi
}
""",
            """pub fnc main(): int {
  const x: owned int = 4;
  if (x) { return x; } fi
  return 0;
}
""",
            """pub fnc main(): int {
  const x: owned int = 4;
  if (x eq 4) { return x; }
  return 0;
}
"""
        ]
        for i, source in enumerate(invalid):
            path = root / f"reject-experimental-{i}.ores"
            path.write_text(source)
            compiler_ir(args.compiler, path, experimental=True, accepted=False)
            total += 1

    print(f"PASS: {total} source-to-Clang executable/rejection cases, "
          "all selected exit paths checked; no implicit heap ABI or tracing GC")


if __name__ == "__main__":
    main()
