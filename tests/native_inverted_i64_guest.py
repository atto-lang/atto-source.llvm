#!/usr/bin/env python3
"""Stepful compiler + native execution proof for the opt-in i64-only source slice.

This test does NOT certify the general inverted struct type checker,
ownership semantics, generational handles, nested records or backend parity.
It compiles actual .ores inputs using oreslang-llvmc, then executes clang IR.
"""
import argparse
import pathlib
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
p.add_argument("--clang", default="clang")
p.add_argument("--fixtures", required=True)
args = p.parse_args()

root = pathlib.Path(args.fixtures)
positive = [
    root / "positive-column-get.ores",
    root / "positive-column-bounded.ores",
]
negative = [
    root / "negative-out-of-bounds.ores",
    root / "negative-two-column-out-of-bounds.ores",
]

def compiler_run(path, experimental=True):
    options = [args.compiler]
    if experimental:
        options.append("--experimental-inverted-i64")
    return subprocess.run(options + [str(path)], capture_output=True,
                          text=True, timeout=40, check=False)

def verify_ir(ir):
    if "define i32 @main()" not in ir or "declare void @llvm.trap()" not in ir:
        raise AssertionError("guest column IR or checked trap missing")
    lanes = ir.count(" = alloca [")
    if lanes not in (1, 2, 3):
        raise AssertionError("expected only physical columns and optional row liveness lane")
    if lanes == 3 and ("%generation = alloca [" not in ir
                       or "%alive = alloca i1" not in ir):
        raise AssertionError("third lane must be checked MIR live-row metadata")
    if "getelementptr inbounds [" not in ir:
        raise AssertionError("no physical column-index addressing")
    if "icmp ult i64" not in ir or "store i64" not in ir or "load i64" not in ir:
        raise AssertionError("missing checked physical store/read execution")
    if "malloc" in ir or "ores_actor_" in ir or "std::vector" in ir:
        raise AssertionError("unexpected actor, GC or host-only modeled storage")

with tempfile.TemporaryDirectory(prefix="ores-inverted-i64-") as scratch:
    temp = pathlib.Path(scratch)
    for i, filename in enumerate(positive):
        normal = compiler_run(filename, experimental=False)
        if normal.returncode == 0 or normal.stdout.lstrip().startswith(";"):
            raise AssertionError("default compiler wrongly admitted experimental inverted source")
        result = compiler_run(filename)
        if result.returncode != 0:
            raise AssertionError(f"real .ores positive failed compilation: {result.stderr}")
        verify_ir(result.stdout)
        if "; typed MIR, experimental local i64 pair LLVM IR" not in result.stdout:
            raise AssertionError("two-i64 guest did not lower through verified typed MIR")
        if "%generation = alloca [" not in result.stdout or (
                "load i1, ptr %alive" not in result.stdout):
            raise AssertionError("typed MIR guest omitted live-row or owner check")
        ir_path, executable = temp / f"positive-{i}.ll", temp / f"positive-{i}"
        ir_path.write_text(result.stdout, encoding="utf-8")
        compile_result = subprocess.run(
            [args.clang, "-x", "ir", str(ir_path), "-o", str(executable)],
            capture_output=True, text=True, timeout=90, check=False)
        if compile_result.returncode:
            raise AssertionError("LLVM IR rejected by clang: " + compile_result.stderr)
        actual = subprocess.run([str(executable)], cwd=temp,
                                capture_output=True, timeout=15, check=False)
        if actual.returncode != 42:
            raise AssertionError("actual compiled guest return expected 42; got "
                                 + str(actual.returncode))
        print("PASS: actual .ores -> physical columns -> LLVM IR -> native guest exit 42",
              filename.name)

    for case_id, bad_source in enumerate(negative):
        bad = compiler_run(bad_source)
        if bad.returncode:
            raise AssertionError(
                "out-of-bounds guest should compile to a runtime trap: "
                + bad.stderr)
        verify_ir(bad.stdout)
        if case_id == 1 and "; typed MIR, experimental local i64 pair LLVM IR" not in bad.stdout:
            raise AssertionError(
                "two-column negative bounds fixture bypassed verified MIR codegen")
        ir_path, executable = temp / f"bounds-{case_id}.ll", temp / f"bounds-{case_id}"
        ir_path.write_text(bad.stdout, encoding="utf-8")
        result = subprocess.run(
            [args.clang, "-x", "ir", str(ir_path), "-o", str(executable)],
            capture_output=True, text=True, timeout=90, check=False)
        if result.returncode:
            raise AssertionError(
                "bounds LLVM IR failed compilation: " + result.stderr)
        execution = subprocess.run(
            [str(executable)], cwd=temp,
            capture_output=True, timeout=15, check=False)
        if execution.returncode == 0 or execution.returncode == 42:
            raise AssertionError(
                "out-of-bounds read was not trapped in " + bad_source.name)

    negative_sources = [
        ("define inverted class P as x:int; end\nfnc main():int{return 0;}",
         "expected 'struct'"),
        ("define inverted struct P as x:float; end\nfnc main():int{return 0;}",
         "expected 'int'"),
        ("define inverted struct P as x:int;x:int;end\nfnc main():int{return 0;}",
         "duplicate inverted field"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(0);return rows.get(0,x);}",
         "capacity must be 1..256"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(257);return rows.get(0,x);}",
         "capacity must be 1..256"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(9223372036854775808);"
         "return rows.get(0,x);}", "i64 integer literal overflow"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(1);"
         "return rows.get(0,unknown);}", "unknown inverted field"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(1);"
         "rows.set(0,x,42);return rows.get(0,x);}",
         "immutable inverted field cannot be written after initialization"),

        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(1);"
         "rows.erase_swap(0);return rows.get(0,x);}",
         "only push and set supported"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=Array<P>.with_capacity(2);return rows.get(0,x);}",
         "expected 'InvertedArray'"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);return rows.get(0,x);}"
         "\nfnc hidden():int{return 1;}", "trailing unsupported inverted code"),

        ("define inverted struct P as x:Pointer<int>; end\n"
         "fnc main():int{return 0;}", "expected 'int'"),
        ("define inverted struct P as x:struct{y:int}; end\n"
         "fnc main():int{return 0;}", "expected 'int'"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(-9223372036854775809);"
         "return rows.get(0,x);}", "i64 integer literal overflow"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);return rows.get(0,x);} /*",
         "unterminated inverted block comment"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);return rows.get(0,x);} "
         + "/*" * 129 + "*/" * 129,
         "inverted block comment too deep"),
        ("define inverted struct P as x:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);return rows.get(0,x);} "
         + " " * 65536,
         "experimental inverted i64 source exceeds 64 KiB"),
        ("define inverted struct P as " +
         "".join(f"f{i}:int;" for i in range(8)) +
         "end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(256);return rows.get(0,f0);}",
         "inverted row cell budget exceeded"),
    ]
    for idx, (source, expected) in enumerate(negative_sources):
        source_file = temp / f"reject-{idx}.ores"
        source_file.write_text(source, encoding="utf-8")
        result = compiler_run(source_file)
        if result.returncode == 0 or "define i32 @main()" in result.stdout:
            raise AssertionError("unsupported inverted source published executable IR")
        if expected not in result.stderr:
            raise AssertionError(f"wrong compiler diagnostic {idx}: {result.stderr!r}")
    print("PASS: 18 unsupported source programs fail closed with compiler-owned diagnostics")
    print("PASS: checked out-of-bounds guest runtime trap")
