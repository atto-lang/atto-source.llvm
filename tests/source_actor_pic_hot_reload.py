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
        test = temp / "hot-source-actor"
        checked([args.cxx, "-std=c++17", "-O1", "-DNDEBUG", *sanitizer,
                 "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                 "-pthread", "-rdynamic", "-I", str(include),
                 str(cxx_source), str(heap_object), "-ldl",
                 "-o", str(test)], timeout=60)
        actual = checked([str(test), *(str(path) for path in paths)],
                         timeout=30)
        if actual.stdout.count("ACTUAL_ORES_SOURCE_AOT_PIC_V1_V2_PASS") != 1:
            raise RuntimeError("native actor PIC v1/v2 success marker missing")
        print("PASS compiler-generated .ores AOT PIC: v1=42 -> v2=86 -> 128")
        print("PASS one actor-owned heap, generation-pinned DSO retirement")
        print("NOT_PROVEN: authenticated production loader, ORC JIT,"
              " await/FFI compiler pins, full source scheduler, process isolation")


if __name__ == "__main__":
    main()
