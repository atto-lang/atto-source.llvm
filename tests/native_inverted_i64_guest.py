#!/usr/bin/env python3
"""Stepful compiler + native execution proof for the opt-in i64-only source slice.

This test does NOT certify the general inverted struct type checker,
ownership semantics, generational handles, nested records or backend parity.
It compiles real .ores (migration) and .atto guest fixtures to LLVM IR,
then executes native Clang binaries. Standalone mode is adapter-only.
"""
import argparse
import pathlib
import signal
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument("--compiler", required=True)
p.add_argument("--clang", default="clang")
p.add_argument("--fixtures", required=True)
p.add_argument("--standalone-mode", action="store_true",
               help="Adapter-only driver; full compiler gate still requires production mode")
args = p.parse_args()

root = pathlib.Path(args.fixtures)
positive = [
    root / "positive-column-get.ores",
    root / "positive-column-bounded.ores",
]
negative = [
    root / "negative-out-of-bounds.ores",
    root / "negative-two-column-out-of-bounds.ores",
    root / "negative-three-column-bounds.atto",
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
    if not 1 <= lanes <= 8:
        raise AssertionError("expected 1..8 physical columns or checked MIR metadata")
    if "; typed MIR, experimental local i64 pair LLVM IR" in ir and (
            lanes != 3 or "%generation = alloca [" not in ir
            or "%alive = alloca i1" not in ir):
        raise AssertionError("two-column typed MIR requires checked live-row metadata")
    if "getelementptr inbounds [" not in ir:
        raise AssertionError("no physical column-index addressing")
    if "icmp ult i64" not in ir or "store i64" not in ir or "load i64" not in ir:
        raise AssertionError("missing checked physical store/read execution")
    if "malloc" in ir or "ores_actor_" in ir or "std::vector" in ir:
        raise AssertionError("unexpected actor, GC or host-only modeled storage")

with tempfile.TemporaryDirectory(prefix="ores-inverted-i64-") as scratch:
    temp = pathlib.Path(scratch)
    for i, filename in enumerate(positive):
        if not args.standalone_mode:
            normal = compiler_run(filename, experimental=False)
            if normal.returncode != 0:
                raise AssertionError("production two-i64 compiler rejected an admitted source: "
                                     + normal.stderr)
            verify_ir(normal.stdout)
            if "; typed MIR, experimental local i64 pair LLVM IR" not in normal.stdout:
                raise AssertionError("production compiler did not emit verified typed MIR")
            if "store volatile i1 false, ptr %alive" not in normal.stdout:
                raise AssertionError("production main did not close and scrub its owner")
        result = compiler_run(filename)
        if result.returncode != 0:
            raise AssertionError(f"real .ores positive failed compilation: {result.stderr}")
        verify_ir(result.stdout)
        if "; typed MIR, experimental local i64 pair LLVM IR" not in result.stdout:
            raise AssertionError("two-i64 guest did not lower through verified typed MIR")
        if "%generation = alloca [" not in result.stdout or (
                "load i1, ptr %alive" not in result.stdout):
            raise AssertionError("typed MIR guest omitted live-row or owner check")
        if "store volatile i1 false, ptr %alive" not in result.stdout:
            raise AssertionError("two-column guest omitted terminal owner revocation")
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

    # Legacy experimental column arities now carry the same explicit terminal
    # owner revocation as the two-column verified-MIR emitter. Production
    # support remains deliberately limited to two i64 columns.
    legacy_positive = [
        ("positive-single-column.atto", 1, 2),
        ("positive-three-columns.atto", 3, 3),
        ("positive-eight-columns.atto", 8, 2),
        # Reuse the three mutation+teardown vectors from superseded #1384.
        # These exercise field-sensitive initialization vs let writes,
        # not just read-only column loads.
        ("positive-one-column-close.atto", 1, 2),
        ("positive-three-column-close.atto", 3, 2),
        ("positive-eight-column-close.atto", 8, 2),
    ]
    for case_id, (name, field_count, capacity) in enumerate(legacy_positive):
        source = root / name
        experimental = compiler_run(source)
        if experimental.returncode != 0:
            raise AssertionError(f"legacy experimental {name} rejected: {experimental.stderr}")
        ir = experimental.stdout
        verify_ir(ir)
        if "; generated by experimental checked inverted i64 guest compiler" not in ir:
            raise AssertionError("legacy guest unexpectedly bypassed its restricted IR adapter")
        if ir.count(" = alloca [") != field_count:
            raise AssertionError("legacy SoA did not allocate one physical column per field")
        if ("%alive = alloca i1" not in ir
                or "store volatile i1 false, ptr %alive" not in ir):
            raise AssertionError("legacy guest did not revoke its logical owner")
        if ir.count("store volatile i64 0, ptr") != field_count * capacity + 1:
            raise AssertionError("legacy guest did not scrub every physical cell and length")
        tail = ir.find("store volatile i64 0, ptr %length")
        ret = ir.find("ret i32 %result")
        if tail < 0 or ret <= tail:
            raise AssertionError("legacy guest returned before terminal owner scrub")
        if not args.standalone_mode:
            production = compiler_run(source, experimental=False)
            if production.returncode == 0 or "define i32 @main()" in production.stdout:
                raise AssertionError("unsupported multicolumn source entered production codegen")
        ir_path = temp / f"legacy-{case_id}.ll"
        ir_path.write_text(ir, encoding="utf-8")
        for opt in ("-O0", "-O2"):
            executable = temp / f"legacy-{case_id}-{opt[1:]}"
            compiled = subprocess.run(
                [args.clang, opt, "-x", "ir", str(ir_path), "-o", str(executable)],
                capture_output=True, text=True, timeout=90, check=False)
            if compiled.returncode:
                raise AssertionError(f"legacy {name} LLVM {opt} rejected: {compiled.stderr}")
            executed = subprocess.run(
                [str(executable)], capture_output=True, timeout=15, check=False)
            if executed.returncode != 42:
                raise AssertionError(
                    f"legacy {name} native {opt} returned {executed.returncode}, expected 42")
        print(f"PASS: {name} source -> bounded SoA -> native Clang O0/O2 exit 42, owner scrub")

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
        if case_id == 2:
            # Three-column reads have a distinct experimental adapter. Check
            # the actual 3 physical columns, guarded index and terminal scrub
            # before executing the checked trap under two optimization levels.
            if "; generated by experimental checked inverted i64 guest compiler" not in bad.stdout:
                raise AssertionError("three-column bounds guest bypassed checked adapter")
            if bad.stdout.count(" = alloca [") != 3:
                raise AssertionError("three-column bounds guest did not emit three lanes")
            if bad.stdout.count("store volatile i64 0, ptr ") != 7:
                raise AssertionError("three-column bounds guest omitted capacity/length scrub")
            if not args.standalone_mode:
                unsupported = compiler_run(bad_source, experimental=False)
                if unsupported.returncode == 0 or "define i32 @main()" in unsupported.stdout:
                    raise AssertionError("production admitted unsupported three-column guest")
        if "icmp ult i64" not in bad.stdout or "call void @llvm.trap()" not in bad.stdout:
            raise AssertionError("bounds guest omitted runtime check/trap")
        ir_path = temp / f"bounds-{case_id}.ll"
        ir_path.write_text(bad.stdout, encoding="utf-8")
        for opt in ("-O0", "-O2"):
            executable = temp / f"bounds-{case_id}-{opt[2:]}"
            result = subprocess.run(
                [args.clang, opt, "-x", "ir", str(ir_path), "-o", str(executable)],
                capture_output=True, text=True, timeout=90, check=False)
            if result.returncode:
                raise AssertionError(
                    f"bounds {opt} LLVM IR failed compilation: " + result.stderr)
            execution = subprocess.run(
                [str(executable)], cwd=temp,
                capture_output=True, timeout=15, check=False)
            # A real SIGILL/SIGTRAP cannot be simulated by a numeric exit code.
            if execution.returncode not in (-signal.SIGILL, -signal.SIGTRAP):
                raise AssertionError(
                    "out-of-bounds read did not signal SIGILL/SIGTRAP in "
                    + bad_source.name + " " + opt + ": " + str(execution.returncode))

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
        ("define inverted struct P as let x:int; id:int;end\nfnc main():int{"
         "let rows=InvertedArray<P>.with_capacity(2);rows.push(1,2);"
         "rows.set(0,id,42);return rows.get(0,x);}",
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
    print(f"PASS: {len(negative_sources)} unsupported source programs fail closed with compiler-owned diagnostics")
    print("PASS: checked out-of-bounds guest runtime trap")
