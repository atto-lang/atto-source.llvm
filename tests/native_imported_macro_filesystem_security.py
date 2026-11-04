#!/usr/bin/env python3
"""Real LLVM compiler imported-macro filesystem denial matrix.

A passing source-only inventory is NOT a substitute for running this test.
Each negative must reject with zero LLVM IR stdout.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def compile_source(compiler: Path, entry: Path):
    return subprocess.run([str(compiler), str(entry)], text=True,
                          capture_output=True, timeout=30, check=False)


def expect_deny(label: str, compiler: Path, entry: Path, diagnostic: str):
    result = compile_source(compiler, entry)
    if (result.returncode == 0 or result.stdout or diagnostic not in result.stderr):
        raise AssertionError(
            f"{label}: expected no emitted IR and {diagnostic!r}; "
            f"got rc={result.returncode}, stdout={result.stdout[:220]!r}, "
            f"stderr={result.stderr[:500]!r}"
        )


def write(path: Path, source: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source, encoding="utf-8")


def consumer(path: Path, target: str):
    write(path,
          f'import * as lib from "./{target}";\n'
          'pub fnc main() -> int { return lib.literal!(const 40+2); }\n')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    if not args.compiler.is_file():
        raise AssertionError("real oreslang-llvmc compiler executable required")

    with tempfile.TemporaryDirectory(prefix="atto-native-macro-source-id-") as tmp:
        root = Path(tmp) / "project"
        producer = root / "producer.atto"
        write(root / ".atto.pkg.toml", '[project]\nroot = "."\n')
        write(producer,
              'define pub macro literal(Const<int> value) -> Syntax<Expr> {\n'
              '  return quote(value);\n}\n')

        alias = root / "alias.atto"
        alias.symlink_to(producer)
        entry = root / "symlink-consumer.atto"
        consumer(entry, "alias.atto")
        expect_deny("in-root source symlink alias", args.compiler, entry,
                    "symlink/alias path")
        alias.unlink()

        second = root / "hardlink.atto"
        os.link(producer, second)
        entry = root / "hardlink-consumer.atto"
        consumer(entry, "producer.atto")
        expect_deny("producer acquires second hardlink name", args.compiler,
                    entry, "hardlink aliases")
        second.unlink()

        entry = root / "missing-consumer.atto"
        consumer(entry, "unknown.atto")
        expect_deny("missing declared macro file", args.compiler, entry,
                    "cannot canonicalize native source path")

        # Never interpret an .atto import as a raw directory, FIFO or legacy
        # extension. The compiler must reject before any IR is emitted.
        if hasattr(os, "mkfifo"):
            fifo = root / "fifo.atto"
            os.mkfifo(fifo)
            entry = root / "fifo-consumer.atto"
            consumer(entry, "fifo.atto")
            expect_deny("nonregular macro source", args.compiler, entry,
                        "regular file")

        print("LLVM_IMPORTED_MACRO_FILESYSTEM_DENIAL_PASS")


if __name__ == "__main__":
    main()
