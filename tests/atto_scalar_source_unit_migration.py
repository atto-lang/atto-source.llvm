#!/usr/bin/env python3
"""Real LLVM source-unit extension migration evidence, no cross-org mirrors."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def require(ok, explanation):
    if not ok:
        raise RuntimeError(explanation)


def execute(args, cwd):
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        result = subprocess.run(args, cwd=cwd, stdin=subprocess.DEVNULL,
                                stdout=out, stderr=err, timeout=25, check=False)
        out.seek(0, 2)
        err.seek(0, 2)
        require(out.tell() <= 2 * 1024 * 1024 and
                err.tell() <= 2 * 1024 * 1024,
                "source-unit compiler exceeded bounded output")
        out.seek(0)
        err.seek(0)
        return result.returncode, out.read(), err.read()


def run(compiler, clang):
    with tempfile.TemporaryDirectory(prefix="atto-source-unit-") as tmp:
        root = Path(tmp)

        def write(name, text):
            path = root / name
            path.write_text(text, encoding="utf-8")
            return path

        # Compilation of a legacy .ores file remains an independently
        # selected compatibility mode. A .atto root cannot retroactively
        # turn explicitly selected .ores imports into canonical syntax.
        write("legacy.ores", (
            "define module Lib as pub fnc answer():int{return 9 % 2;} end\n"))
        entry = write("main.atto", (
            'import module Lib from "./legacy.ores"; '
            "pub fnc main():int{return Lib.answer();}\n"))
        rc, ir, err = execute([str(compiler), str(entry)], root)
        require(rc == 0 and b"define i32 @main(" in ir and err == b"",
                "explicit .ores compatibility import lost: " + repr(err[:300]))
        ir_path = root / "compat.ll"
        ir_path.write_bytes(ir)
        binary = root / "compat"
        rc, out, err = execute([str(clang), "-O0", "-x", "ir", str(ir_path),
                                "-o", str(binary)], root)
        require(rc == 0, "compatibility native link failed: " + repr(err[:300]))
        rc, out, err = execute([str(binary)], root)
        require((rc, out, err) == (1, b"", b""),
                "legacy remainder guest no longer returns integer 1")

        # Importer suffix NEVER weakens the imported canonical unit.
        write("canonical.atto", (
            "define module Lib as pub fnc answer():int{return 9 % 2;} end\n"))
        legacy_root = write("entry.ores", (
            'import module Lib from "./canonical.atto"; '
            "pub fnc main():int{return Lib.answer();}\n"))
        rc, out, err = execute([str(compiler), str(legacy_root)], root)
        require(rc == 1 and out == b"" and
                b"ATTO_REMOVED_OPERATOR" in err,
                "retired operator laundered through .ores import")

        # Comments containing retired punctuation are not source operators.
        comments = write("comments.atto", (
            "pub fnc main():int{ /* % ^ ^^ */ return 42; } // ^ %\n"))
        rc, ir, err = execute([str(compiler), str(comments)], root)
        require(rc == 0 and b"define i32 @main(" in ir and err == b"",
                "operator in a comment was rejected or ignored source: " +
                repr(err[:300]))
        ir_path.write_bytes(ir)
        rc, out, err = execute([str(clang), "-O0", "-x", "ir", str(ir_path),
                                "-o", str(binary)], root)
        require(rc == 0, "comment native linker failed: " + repr(err[:300]))
        rc, out, err = execute([str(binary)], root)
        require((rc, out, err) == (42, b"", b""),
                "comment-only retired operators changed typed guest")

    print("ATTO_LLVM_SOURCE_UNIT_MIGRATION_NATIVE_EXECUTED "
          "compat_ores=1 imported_atto_denied=1 comment_guest=42")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    args = parser.parse_args()
    require(not args.compiler.is_symlink(), "symlinked native compiler")
    compiler = args.compiler.resolve(strict=True)
    clang = args.clang.resolve(strict=True)
    require(compiler.is_file() and clang.is_file(),
            "missing native compiler/clang")
    run(compiler, clang)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as exc:
        print("ATTO_SOURCE_UNIT_MIGRATION_DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
