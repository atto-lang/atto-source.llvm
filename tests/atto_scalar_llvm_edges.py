#!/usr/bin/env python3
"""Supplemental LLVM .atto scalar grammar and ownership native regressions.

Separate from byte-pinned normative 19-case corpus. Real LLVM guest
execution is required for each positive; denied source must emit no LLVM IR.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

from atto_scalar_subprocess import run_bounded

POSITIVE = (
    ("two-readonly-locals", "pub fnc main():int{int x=20;const int y=22;return x+y;}", 42),
    ("mutable-explicit-rebind", "pub fnc main():int{let int x=7;x=42;return x;}", 42),
    ("inferred-literal", "pub fnc main():int{const x=7;return x*6;}", 42),
    ("mixed-value-formals", "fnc add(x:int,int y):int{return x+y;}pub fnc main():int{return add(20,22);}", 42),
    ("grouped-type-first", "pub fnc main():int{int x=(2+3)*4;return x+22;}", 42),
    ("retired-symbols-in-comments", "pub fnc main():int{/* ^^ % ^ */return 42;} // ^^ % ^\n", 42),
    ("branch-inferred-const", "pub fnc main():int{if(true){const x=7;return x*6;} fi return 0;}", 42),
)
NEGATIVE = (
    ("duplicate-formal", "fnc add(x:int,int x):int{return x;}pub fnc main():int{return add(20,22);}", "duplicate parameter"),
    ("duplicate-local", "pub fnc main():int{const x=7;const x=8;return x;}", "duplicate owned local"),
    ("inferred-const-rebind", "pub fnc main():int{const x=7;x=42;return x;}", "assignment requires an unloaned mutable scalar slot"),
    ("type-first-rebind", "pub fnc main():int{int x=7;x=42;return x;}", "assignment requires an unloaned mutable scalar slot"),
    ("grouped-modulo", "pub fnc main():int{return (9 % 2);}", "ATTO_REMOVED_OPERATOR"),
    ("grouped-double-caret", "pub fnc main():int{return (2 ^^ 3);}", "ATTO_REMOVED_OPERATOR"),
    ("caret-after-add", "pub fnc main():int{return 2 + 3 ^ 4;}", "ATTO_REMOVED_OPERATOR"),
    ("i64-positive-overrange", "pub fnc main():int{return 9223372036854775808;}", "overflows signed 64-bit result"),
    ("i64-negative-overrange", "pub fnc main():int{return -9223372036854775809;}", "overflows signed 64-bit result"),
    ("no-bool-int-coercion", "pub fnc main():int{return true+2;}", None),
    ("branch-inferred-const-rebind", "pub fnc main():int{if(true){const x=7;x=8;return x;} fi return 0;}", "assignment requires an unloaned mutable scalar slot"),
    ("branch-inferred-nonliteral", "pub fnc main():int{if(true){const x=40+2;return x;} fi return 0;}", "native inferred const requires a checked int literal initializer"),
)


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    require(not args.compiler.is_symlink() and args.compiler.is_file(),
            "missing or symlinked LLVM native compiler")
    require(args.clang.is_file(), "missing Clang binary")
    compiler = args.compiler.resolve(strict=True)
    clang = args.clang.resolve(strict=True)
    require(len({case[0] for case in POSITIVE + NEGATIVE}) ==
            len(POSITIVE + NEGATIVE), "duplicate supplemental test ID")
    with tempfile.TemporaryDirectory(prefix="atto-llvm-scalar-edges-") as temporary:
        root = Path(temporary)
        for name, source, expected in POSITIVE:
            work = root / name
            work.mkdir()
            path = work / "main.atto"
            path.write_text(source, encoding="utf-8")
            rc, ir, stderr = run_bounded([str(compiler), str(path)], work, timeout=25)
            require(rc == 0 and ir.startswith(b"; ModuleID") and stderr == b"",
                    name + " rejected by compiler: " + repr(stderr[:300]))
            for optimization in ("-O0", "-O2"):
                suffix = optimization[1:]
                ll = work / ("main-" + suffix + ".ll")
                ll.write_bytes(ir)
                native = work / ("native-" + suffix)
                rc, out, stderr = run_bounded([str(clang), optimization, "-x", "ir",
                                                str(ll), "-o", str(native)], work, timeout=30)
                require(rc == 0 and native.is_file(),
                        name + " " + optimization + " rejected by Clang: " + repr(stderr[:300]))
                rc, out, stderr = run_bounded([str(native)], work, timeout=10)
                require((rc, out, stderr) == (expected, b"", b""),
                        name + " " + optimization + ": guest result not equal to " + str(expected))
        for name, source, diagnostic in NEGATIVE:
            work = root / name
            work.mkdir()
            path = work / "main.atto"
            path.write_text(source, encoding="utf-8")
            rc, out, stderr = run_bounded([str(compiler), str(path)], work, timeout=25)
            require(rc == 1 and out == b"" and 0 < len(stderr) <= 65536 and
                    (diagnostic is None or diagnostic.encode() in stderr),
                    name + ": invalid source accepted or incorrect diagnosis: " + repr(stderr[:300]))
            require([f.name for f in work.iterdir()] == ["main.atto"],
                    name + ": denied compiler wrote an output artifact")
    print(f"ATTO_LLVM_SCALAR_EDGES_GUEST_EXECUTED:{len(POSITIVE) * 2} "
          f"NEGATIVES_REJECTED:{len(NEGATIVE)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, ValueError, subprocess.TimeoutExpired) as error:
        print("ATTO_LLVM_SCALAR_EDGES_DENIED: " + str(error), file=sys.stderr)
        sys.exit(1)
