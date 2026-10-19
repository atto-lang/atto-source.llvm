#!/usr/bin/env python3
"""Actual .ores -> LLVM IR -> PIC DSOs -> native code generation gate.

Only a test-built, private temp directory is used for dlopen. This does NOT
authenticate externally supplied code, certify a production loader, run ORC
JIT, or execute the full source actor scheduler. Real guest on_message/init
functions and the actual actor-owned i64 heap execute here.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import os
import re

# Native source-actor v1 is intentionally a deliberately limited ABI:
# 3 scalar fields in fixed order. A position-preserving field rename still
# changes source schema and MUST fail rather than relying on C++ hard-coded
# `shape()` metadata as its own evidence.
EXPECTED_FIELDS = (
    ("const", "base", "int"),
    ("val", "delta", "int"),
    ("let", "total", "int"),
)


def source_actor_layout(source_text):
    """Fail closed on any unrecognised actor field header (test-only oracle).

    This is NOT an authenticated compiler-produced ABI manifest and cannot
    validate arbitrary Oreslang syntax. It refuses grammar drift instead of
    silently accepting a source layout the test does not understand.
    """
    before_handler = source_text.split("pub fnc on_message(", 1)
    if len(before_handler) != 2:
        raise ValueError("source actor handler not uniquely identified")
    if before_handler[1].count("pub fnc on_message("):
        raise ValueError("multiple actor message handlers")
    lines = [
        line.strip() for line in before_handler[0].splitlines()
        if line.strip() and not line.lstrip().startswith("//")
    ]
    if not lines or lines.pop(0) != "define actor Counter as {":
        raise ValueError("unexpected actor type or declaration")
    fields = []
    for line in lines:
        match = re.fullmatch(
            r"(const|val|let) ([A-Za-z_][A-Za-z0-9_]*): "
            r"(int) = [^;]+;", line
        )
        if match is None:
            raise ValueError("unknown or reordered actor field declaration")
        fields.append(match.groups())
    if tuple(fields) != EXPECTED_FIELDS:
        raise ValueError("actor field ABI changed without migration")
    return tuple(fields)


def validate_source_fixture_pair(v1, v2):
    first = source_actor_layout(v1)
    second = source_actor_layout(v2)
    if first != second:
        raise ValueError("source-generated actor layouts differ")
    # This is a harness self-check: no passing test may depend solely on
    # hard-coded metadata while its own fixture inspector silently drifts.
    for mutated in (
        v2.replace("let total: int", "let renamed: int", 1),
        v2.replace("let total: int", "val total: int", 1),
        v2.replace("let total: int", "let total: string", 1),
    ):
        if mutated == v2:
            raise ValueError("layout mutation selftest fixture missing")
        try:
            source_actor_layout(mutated)
        except ValueError:
            pass
        else:
            raise ValueError("layout mutation falsely accepted")


def checked(command, *, timeout=60):
    run = subprocess.run(command, capture_output=True, text=True,
                         timeout=timeout, check=False)
    if run.returncode != 0:
        raise RuntimeError(
            f"command failed ({run.returncode}): {command!r}\n"
            f"stdout={run.stdout[:4000]}\nstderr={run.stderr[:4000]}"
        )
    return run


def main():
    cli = argparse.ArgumentParser()
    for name in ("compiler", "clang", "cxx", "source-root"):
        cli.add_argument("--" + name, required=True)
    cli.add_argument("--sanitizers", action="store_true")
    args = cli.parse_args()
    source_root = Path(args.source_root).resolve(strict=True)
    runtime = source_root / "src/main/cpp/oreslang/runtime"
    include = source_root / "src/main/cpp"
    fixtures = source_root / "tests/fixtures"
    cxx_source = source_root / "tests/native_source_actor_pic_hot_reload_test.cpp"
    for file in (cxx_source, runtime / "ActorOwnedI64Heap.c"):
        if not file.is_file():
            raise RuntimeError(f"missing trusted native fixture source: {file}")

    sanitizer = (["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
                 if args.sanitizers else [])
    first_source = (fixtures / "actor-hot-reload-aot-v1.ores").read_text(
        encoding="utf-8"
    )
    second_source = (fixtures / "actor-hot-reload-aot-v2.ores").read_text(
        encoding="utf-8"
    )
    validate_source_fixture_pair(first_source, second_source)

    with tempfile.TemporaryDirectory(prefix="ores-actor-pic-") as directory:
        temp = Path(directory)
        paths = []
        for version in ("v1", "v2"):
            source = fixtures / f"actor-hot-reload-aot-{version}.ores"
            guest = checked([args.compiler, str(source)], timeout=30)
            if not guest.stdout.startswith("; ModuleID"):
                raise RuntimeError(f"{version}: no real LLVM IR from compiler")
            for symbol in ("oreslang_actor_Counter_init",
                           "oreslang_actor_Counter_on_message"):
                # Merely declaring (or calling) the symbol is insufficient:
                # the compiler must emit the ACTUAL source-defined body.
                if not any(line.startswith("define ")
                           and ("@" + symbol + "(") in line
                           for line in guest.stdout.splitlines()):
                    raise RuntimeError(
                        f"{version}: missing compiler-defined actor ABI: {symbol}")
            ir = temp / f"{version}.ll"
            ir.write_text(guest.stdout, encoding="utf-8")
            shared = temp / f"counter-{version}.so"
            # Native runtime heap symbols intentionally resolve from the
            # host process; the test host is compiled with -rdynamic.
            checked([args.clang, "-O1", "-fPIC", "-shared", *sanitizer,
                     "-x", "ir", str(ir), "-o", str(shared)], timeout=60)
            paths.append(shared)

        heap_object = temp / "heap.o"
        checked([args.clang, "-std=c11", "-O1", "-fPIC", *sanitizer,
                 "-I", str(include), "-c",
                 str(runtime / "ActorOwnedI64Heap.c"),
                 "-o", str(heap_object)], timeout=60)
        scheduler_object = temp / "scheduler.o"
        checked([args.clang, "-std=c11", "-O1", "-fPIC", *sanitizer,
                 "-pthread", "-I", str(include), "-c",
                 str(runtime / "NativeActorScheduler.c"),
                 "-o", str(scheduler_object)], timeout=60)
        test = temp / "hot-source-actor"
        checked([args.cxx, "-std=c++17", "-O1", "-DNDEBUG", *sanitizer,
                 "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-pthread", "-rdynamic", "-I", str(include),
                 str(cxx_source), str(heap_object), str(scheduler_object), "-ldl",
                 "-o", str(test)], timeout=60)
        actual = checked([str(test), *(str(path) for path in paths)],
                         timeout=30)
        if actual.stdout.count("ACTUAL_ORES_SOURCE_AOT_PIC_V1_V2_PASS") != 1:
            raise RuntimeError("native actor PIC v1/v2 success marker missing")
        print("PASS compiler-generated .ores AOT PIC: v1=42 -> v2=86 -> 128")
        print("PASS one actor-owned heap, generation-pinned DSO retirement")
        print("PASS independent fixture field-layout refusal and 3 mutations")
        print("NOT_PROVEN: authenticated production loader, ORC JIT,"
              " await/FFI compiler pins, full source scheduler, process isolation")


if __name__ == "__main__":
    main()
