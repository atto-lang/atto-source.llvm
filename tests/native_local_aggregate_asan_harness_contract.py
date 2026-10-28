#!/usr/bin/env python3
"""Source-free proof that ASan flags alone do not instrument pre-generated LLVM IR."""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    if not args.clang:
        raise AssertionError("ASAN_PROBE_CLANG_REQUIRED")

    target = Path(__file__).with_name("native_local_aggregate_parity_stage1.py")
    spec = importlib.util.spec_from_file_location("local_aggregates", target)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    source = (
        "; ModuleID = 'local-aggregate-sanitizer-probe'\n"
        'source_filename = "local-aggregate-sanitizer-probe"\n'
        "define i32 @main() {\n"
        "entry:\n"
        "  %slot = alloca i32, align 4\n"
        "  store volatile i32 7, ptr %slot, align 4\n"
        "  %value = load volatile i32, ptr %slot, align 4\n"
        "  ret i32 %value\n"
        "}\n"
    )

    with tempfile.TemporaryDirectory(prefix="ores-asan-ir-probe-") as dirname:
        directory = Path(dirname)
        raw = directory / "raw.ll"
        raw.write_text(source, encoding="utf-8")
        # Raw .ll + Clang -fsanitize=address must NOT satisfy the access check.
        try:
            mod.verify_native_ir_asan(args.clang, raw, directory)
        except AssertionError as exc:
            if "LOCAL_AGGREGATE_ASAN_INSTRUMENTATION_MISSING" not in str(exc):
                raise
        else:
            raise AssertionError("UNINSTRUMENTED_IR_FALSE_ASAN_PASS")

        marked = directory / "marked.ll"
        instrumented = mod.mark_native_ir_for_asan(source)
        if instrumented.count("sanitize_address {") != 1:
            raise AssertionError("ASAN_FUNCTION_ATTRIBUTE_MISSING")
        marked.write_text(instrumented, encoding="utf-8")
        mod.verify_native_ir_asan(args.clang, marked, directory)

        executable = directory / "marked"
        linked = subprocess.run(
            [args.clang, "-fsanitize=address", str(marked), "-o", str(executable)],
            capture_output=True, text=True, timeout=50)
        if linked.returncode:
            raise AssertionError(("ASAN_PROBE_LINK_FAILED", linked.stderr[:1000]))
        completed = subprocess.run([str(executable)], capture_output=True,
                                   text=True, timeout=20)
        if completed.returncode != 7:
            raise AssertionError(("ASAN_PROBE_WRONG_EXIT", completed.returncode,
                                  completed.stderr[:1000]))

    try:
        mod.mark_native_ir_for_asan("declare i32 @main()\n")
    except AssertionError as exc:
        if "LOCAL_AGGREGATE_ASAN_ATTRIBUTE_GATE" not in str(exc):
            raise
    else:
        raise AssertionError("ASAN_EMPTY_DEFINITION_FALSE_PASS")
    print("PASS raw-IR rejection, ASan memory-access instrumentation, native exit and no-definition rejection")


if __name__ == "__main__":
    main()
