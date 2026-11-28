#!/usr/bin/env python3
"""Real LLVM guest execution for bounded value elif, not a host evaluator."""
import argparse
from pathlib import Path
import subprocess
import tempfile

POS = [
    ("first", "return if true then 42 elif false then 1 else 2 fi;"),
    ("middle", "return if false then 1 elif true then 42 else 2 fi;"),
    ("last", "return if false then 1 elif false then 2 else 42 fi;"),
    ("nested", "return if true then if false then 1 elif true then 42 else 2 fi else 3 fi;"),
    ("ternary", "return true ? if false then 1 elif true then 42 else 2 fi : 3;"),
    ("negated", "return unless true then 1 elif true then 42 else 2 fi;"),
    ("limit", "return if false then 1 " + "elif false then 2 " * 16 + "else 42 fi;"),
]
NEG = [
    ("missing_else", "return if false then 1 elif true then 42 fi;"),
    ("nonbool", "return if true then 42 elif 1 then 2 else 3 fi;"),
    ("undefined_arm", "return if true then 42 elif false then unknown_name else 3 fi;"),
    ("mixed_type", "return if true then 42 elif false then true else 3 fi;"),
    ("bad_delimiter", "return if false then 1 elif true do 42 else 2 fi;"),
    ("excess", "return if false then 1 " + "elif false then 2 " * 17 + "else 42 fi;"),
]

def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=75)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="atto-llvm-value-elif-") as temp:
        root = Path(temp)
        for name, body in POS:
            src = root / (name + ".atto")
            src.write_text("pub fnc main(): int { " + body + " }\n", encoding="utf-8")
            result = run([args.compiler, str(src)])
            assert result.returncode == 0 and result.stdout, (name, result.stderr)
            assert "phi i64" in result.stdout, name
            ir = root / (name + ".ll")
            ir.write_text(result.stdout, encoding="utf-8")
            for opt in ["-O0", "-O2"]:
                exe = root / (name + opt)
                built = run([args.clang, opt, "-x", "ir", str(ir), "-o", str(exe)])
                assert built.returncode == 0, (name, opt, built.stderr)
                guest = run([str(exe)])
                assert guest.returncode == 42 and not guest.stderr, (name, opt, guest)
        for name, body in NEG:
            src = root / (name + ".atto")
            src.write_text("pub fnc main(): int { " + body + " }\n", encoding="utf-8")
            result = run([args.compiler, str(src)])
            assert result.returncode > 0 and result.stderr and not result.stdout, (name, result)
    print(f"LLVM_VALUE_ELIF native_guests={len(POS)*2} no_ir_rejects={len(NEG)} failures=0")

if __name__ == "__main__":
    main()
