#!/usr/bin/env python3
"""Exact compiler-backed .atto logical-identity gate; never a stub pass."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


MANIFEST = 'schema_version = "1"\n[project]\nroot = "."\n'
ENTRY = 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}'
LIB = 'define module Lib as pub fnc answer():int{return 42;} end'


def run(argv):
    return subprocess.run(argv, text=True, capture_output=True, timeout=30, check=False)


def must_reject(compiler, directory, label, expected):
    result = run([compiler, str(directory / "main.atto")])
    if result.returncode != 1 or result.stdout or expected not in result.stderr:
        raise AssertionError(
            (label, "expected clean reject", expected, result.returncode,
             result.stdout[:200], result.stderr[:2000]))
    if list(directory.rglob("*.ll")) or list(directory.rglob("*.o")):
        raise AssertionError((label, "compiler left an artifact after rejection"))


def write_program(directory, entry=ENTRY, lib=LIB):
    directory.mkdir(parents=True)
    (directory / "main.atto").write_text(entry, encoding="utf-8")
    (directory / ".atto.pkg.toml").write_text(MANIFEST, encoding="utf-8")
    (directory / "lib.atto").write_text(lib, encoding="utf-8")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve(strict=True))
    clang = str(Path(args.clang).resolve(strict=True))
    passed = 0

    with tempfile.TemporaryDirectory(prefix="atto-logical-unit-native-") as tmp:
        base = Path(tmp)

        valid = base / "valid"
        write_program(valid)
        compiled = run([compiler, str(valid / "main.atto")])
        if compiled.returncode != 0 or compiled.stderr or "define " not in compiled.stdout:
            raise AssertionError(("real compiler linked-positive", compiled.returncode, compiled.stderr))
        ir = valid / "checked.ll"
        ir.write_text(compiled.stdout, encoding="utf-8")
        for opt in ("0", "2"):
            executable = valid / ("guest-o" + opt)
            linked = run([clang, "-O" + opt, str(ir), "-o", str(executable)])
            if linked.returncode != 0:
                raise AssertionError(("real clang link", opt, linked.stderr))
            executed = run([str(executable)])
            if executed.returncode != 42 or executed.stdout or executed.stderr:
                raise AssertionError(("native guest", opt, executed.returncode,
                                      executed.stdout, executed.stderr))
            passed += 1

        escape = base / "escape"
        write_program(escape / "project",
                      'import module Lib from "../outside.atto"; pub fnc main():int{return Lib.answer();}')
        (escape / "outside.atto").write_text(LIB, encoding="utf-8")
        must_reject(compiler, escape / "project", "project root escape", "ATTO_UNIT_ROOT_ESCAPE")
        passed += 1

        nested = base / "nested"
        write_program(nested, 'import module Lib from "./child/lib.atto"; pub fnc main():int{return Lib.answer();}')
        (nested / "child").mkdir()
        (nested / "child" / "lib.atto").write_text(LIB, encoding="utf-8")
        (nested / "child" / ".atto.pkg.toml").write_text(MANIFEST, encoding="utf-8")
        must_reject(compiler, nested, "nested package", "ATTO_UNIT_NESTED_PACKAGE")
        passed += 1

        aliased = base / "hardlink"
        write_program(aliased)
        os.link(aliased / "lib.atto", aliased / "duplicate.atto")
        must_reject(compiler, aliased, "hardlinked import", "ATTO_UNIT_SOURCE_ALIAS")
        passed += 1

        symlink = base / "symlink"
        write_program(symlink)
        (symlink / "lib.atto").unlink()
        (symlink / "lib.atto").symlink_to(symlink / "other.atto")
        (symlink / "other.atto").write_text(LIB, encoding="utf-8")
        must_reject(compiler, symlink, "symlink import", "symlinked AttoLang source path")
        passed += 1

    print(f"ATTO_LOGICAL_UNIT_LINKED_GUEST_PASS checks={passed} real_compiler=1")


if __name__ == "__main__":
    main()
