#!/usr/bin/env python3
"""Actual oreslang-llvmc -> Clang -> native guest checks for root-owned IDs.

This is ordinary imported-module execution, NOT imported macro expansion.
No source-free or stub compiler may satisfy the real LLVM guest CTest.
"""
import argparse
import shutil
from pathlib import Path
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=35,
                          check=False)


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def compile_guest(compiler, path):
    return run([str(compiler), str(path)])


def expect_rejected(compiler, case, entry, diagnostic):
    result = compile_guest(compiler, entry)
    if result.returncode == 0 or result.stdout or diagnostic not in result.stderr:
        raise AssertionError(
            f"{case}: expected diagnostic={diagnostic!r}, no LLVM IR, "
            f"got rc={result.returncode} stdout={result.stdout[:160]!r} "
            f"stderr={result.stderr[:400]!r}"
        )


def expect_guest(compiler, clang, case, entry, expected_exit):
    result = compile_guest(compiler, entry)
    if result.returncode != 0 or "@main(" not in result.stdout:
        raise AssertionError(f"{case}: real compiler did not emit linked main IR: "
                             f"{result.stderr[:500]!r}")
    output = entry.parent / "guest.ll"
    binary = entry.parent / "guest"
    output.write_text(result.stdout, encoding="utf-8")
    built = run([str(clang), str(output), "-O2", "-o", str(binary)])
    if built.returncode != 0:
        raise AssertionError(f"{case}: clang failed: {built.stderr[:500]!r}")
    result = run([str(binary)])
    if result.returncode != expected_exit or result.stdout or result.stderr:
        raise AssertionError(f"{case}: wrong native exit/stdout/stderr: "
                             f"rc={result.returncode} out={result.stdout!r} "
                             f"err={result.stderr!r}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", type=Path, required=True)
    p.add_argument("--clang", type=Path, required=True)
    args = p.parse_args()
    if not args.compiler.is_file() or not shutil.which(str(args.clang)):
        raise AssertionError("real source compiler and clang paths are required")

    with tempfile.TemporaryDirectory(prefix="atto-llvm-logical-id-") as tmp:
        root = Path(tmp)
        project = root / "owned"
        write(project / ".atto.pkg.toml", '[project]\nroot = "."\n')
        producer = project / "producer.atto"
        consumer = project / "app" / "consumer.atto"
        write(producer, "define module Lib as pub fnc answer():int{return 40+2;} end")
        write(consumer, 'import module Lib from "../producer.atto"; '
                        'pub fnc main():int{return Lib.answer();}')
        expect_guest(args.compiler, args.clang, "project rooted linked .atto", consumer, 42)

        # Explicit legacy imports are still allowed; there is no ambient
        # extensionless downgrade to a legacy sibling in this positive.
        write(project / "legacy.ores",
              "define module Legacy as pub fnc answer():int{return 42;} end")
        legacy = project / "legacy_consumer.atto"
        write(legacy, 'import module Legacy from "./legacy.ores"; '
                      'pub fnc main():int{return Legacy.answer();}')
        expect_guest(args.compiler, args.clang, "explicit legacy .ores", legacy, 42)

        # Without an explicitly declared package root, '../' is not authority
        # to ingest a sibling project merely because a canonical path exists.
        unrelated = root / "unowned"
        write(unrelated / "outside.atto",
              "define module Escape as pub fnc answer():int{return 42;} end")
        entry = unrelated / "app" / "consumer.atto"
        write(entry, 'import module Escape from "../../owned/producer.atto"; '
                     'pub fnc main():int{return 1;}')
        expect_rejected(args.compiler, "unowned import", entry,
                        "outside declared source roots")

        alias = project / "alias.atto"
        try:
            alias.symlink_to(producer)
        except (OSError, NotImplementedError):
            if __import__("os").name != "nt":
                raise
        else:
            entry = project / "alias_consumer.atto"
            write(entry, 'import module Lib from "./alias.atto"; '
                         'pub fnc main():int{return Lib.answer();}')
            expect_rejected(args.compiler, "in-root symlink alias", entry,
                            "symlink/alias path")

        hardlink = project / "hardlink.atto"
        try:
            hardlink.hardlink_to(producer)
        except (OSError, NotImplementedError):
            if __import__("os").name != "nt":
                raise
        else:
            entry = project / "hardlink_consumer.atto"
            write(entry, 'import module Lib from "./producer.atto"; '
                         'pub fnc main():int{return Lib.answer();}')
            expect_rejected(args.compiler, "unseen hardlink alias", entry,
                            "hardlink")
            hardlink.unlink()

        print("ATTO_LLVM_LOGICAL_ID_GUEST_42_AND_NEGATIVES_PASS")


if __name__ == "__main__":
    main()
