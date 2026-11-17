#!/usr/bin/env python3
"""Execute REAL oreslang-llvmc -> native LLVM IR -> clang -> program.

No source fixture is interpreted by Python. All positive/negative programs
are newly authored in this compiler repository, not cross-org copied.
"""
import argparse
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from guard_artifact_evidence import require_no_rejected_artifacts

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--clang", required=True)
arguments, remaining = parser.parse_known_args()
COMPILER = Path(arguments.compiler).resolve()
CLANG = Path(arguments.clang).resolve()
assert COMPILER.is_file() and os.access(COMPILER, os.X_OK), "REAL_COMPILER_REQUIRED"
assert CLANG.is_file() and os.access(CLANG, os.X_OK), "REAL_CLANG_REQUIRED"

def run(args, **kwargs):
    return subprocess.run(args, capture_output=True, timeout=90, **kwargs)

def program(guard, value=42):
    return "pub fnc main(): int {\n" + guard + "\n return " + str(value) + ";\n}\n"

POSITIVE = [
    ("caret_task_await_guard_fallthrough",
     "pub suspends fnc main(): int { let Task<int> pending = task { ^ 41; }; "
     "val answer: int = await pending; guard answer > 0 { ^ -1; } ^ answer + 1; }", 42),
    ("caret_task_await_guard_failure_returns",
     "pub suspends fnc main(): int { let Task<int> pending = task { ^ 41; }; "
     "val answer: int = await pending; guard answer < 0 { ^ -1; } ^ answer; }", -1),
    ("caret_return_statement", "pub fnc main(): int { ^ 42; }", 42),
    ("caret_guard_false_exits", program("guard false { ^ 7; }"), 7),
    ("caret_guard_true_falls_through", "pub fnc main(): int { guard true { ^ 7; } ^ 42; }", 42),
    ("caret_guard_in_helper", "fnc pick(bool ready): int { guard ready { ^ 7; } ^ 42; } "
                              "pub fnc main(): int { ^ pick(false); }", 7),

    ("literal_false_exits", program("guard false { return 7; }"), 7),
    ("literal_true_falls_through", program("guard true { return 7; }"), 42),
    ("positive_numeric_comparison", program("guard 5 > 0 { return 7; }"), 42),
    ("zero_numeric_comparison", program("guard 0 > 0 { return 7; }"), 7),
    ("false_and_repeated_guard", program("guard true { return 5; }\n guard false { return 9; }"), 9),
    ("all_guard_pass", program("guard true { return 5; }\n guard true { return 9; }"), 42),
    ("legacy_if_true_unchanged", program("if true { return 7; } fi"), 7),
    ("legacy_if_false_unchanged", program("if false { return 7; } fi"), 42),
    ("boolean_helper_false", "fnc pick(bool ready): int { guard ready { return 7; } return 42; }\n"
                             "pub fnc main(): int { return pick(false); }\n", 7),
    ("boolean_helper_true", "fnc pick(bool ready): int { guard ready { return 7; } return 42; }\n"
                            "pub fnc main(): int { return pick(true); }\n", 42),
    ("int_helper_comparison", "fnc pick(int value): int { guard value > 0 { return 7; } return 42; }\n"
                             "pub fnc main(): int { return pick(0); }\n", 7),
    ("portable_identity_helper", "fnc identity(int value): int { return value; }\n"
                                 "pub fnc main(): int { return identity(42); }\n", 42),
    ("guard_uses_portable_helper", "fnc identity(int value): int { return value; }\n"
                                  "fnc choose(bool ready): int { guard ready { return identity(7); } return identity(42); }\n"
                                  "pub fnc main(): int { return choose(false); }\n", 7),
]
NEGATIVE = [
    ("task_future_await_twice",
     "pub suspends fnc main(): int { let Task<int> pending = task { ^ 41; }; "
     "val first: int = await pending; val second: int = await pending; ^ second; }", None),
    ("caret_infix_cannot_xor", "pub fnc main(): int { ^ 41 ^ 1; }", None),
    ("caret_cannot_be_expression", "pub fnc main(): int { return ^ 42; }", None),
    ("double_caret_cannot_return", "pub fnc main(): int { ^^ 42; }", None),
    ("caret_nonvoid_requires_value", "pub fnc main(): int { ^; }", None),
    ("caret_guard_requires_one_terminal_exit", "pub fnc main(): int { guard false { ^ 7; ^ 8; } ^ 42; }", None),

    ("int_truthiness", program("guard 1 { return 7; }"), "bool"),
    ("int_param_truthiness", "fnc f(int x): int { guard x { return 7; } return 42; }\n"
                             "pub fnc main(): int { return f(1); }\n", "bool"),
    ("no_guard_return", program("guard true { 7; }"), None),
    ("no_final_return", "pub fnc main(): int { guard true { return 7; } }\n", None),
    ("guard_else", program("guard false { return 7; } else { return 9; }"), None),
    ("not_a_terminal_body", program("guard false { return 7; return 9; }"), None),
    ("bad_return_type", program("guard false { return true; }"), "int"),
    ("guard_limit", program("guard true { return 7; }\n" * 9), "8"),
    ("unclosed_guard", program("guard false { return 7; "), None),
    ("unsupported_suspend", program("guard true { await fake; return 7; }"), None),
    ("unlowered_trait_decl", "define trait Show as { fnc show(int value): int { return value; } }\n"
                             "pub fnc main(): int { return 7; }\n", None),
    ("unlowered_typeclass_implementation", "impl Show for int { fnc show(int value): int { return value; } }\n"
                                           "pub fnc main(): int { return 7; }\n", None),
]

class ActualNativeGuard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix="atto-real-guard-")
        cls.work = Path(cls.workspace.name)
        cls.compiler_digest = hashlib.sha256(COMPILER.read_bytes()).hexdigest()
        print("ATTO_NATIVE_GUARD_COMPILER_SHA256="+cls.compiler_digest, flush=True)
    @classmethod
    def tearDownClass(cls):
        cls.workspace.cleanup()
    def compile(self, case, source):
        self.assertLess(len(source.encode()),4096)
        # Isolate every compiler invocation. Negative cases must prove the
        # compiler did not write IR, object files, or an executable *anywhere*
        # in its case workspace; checking a never-created .native path alone
        # gives false confidence.
        case_dir = self.work/case
        case_dir.mkdir()
        path=case_dir/"input.atto"
        path.write_text(source,encoding="utf-8",newline="\n")
        before=hashlib.sha256(path.read_bytes()).hexdigest()
        result=run([str(COMPILER),"--target=host",str(path)],
                   cwd=case_dir, stdin=subprocess.DEVNULL)
        self.assertEqual(before,hashlib.sha256(path.read_bytes()).hexdigest(),"compiler modified source")
        return result, path
    def test_compiler_native_positive(self):
        for name,source,expected in POSITIVE:
            with self.subTest(name=name):
                result,path=self.compile(name,source)
                self.assertEqual(result.returncode,0,
                  "real compiler refused "+name+": "+result.stderr.decode(errors="replace")[-1200:])
                self.assertIn(b"define ",result.stdout,"no compiler-generated LLVM IR")
                ir=self.work/(name+".ll")
                ir.write_bytes(result.stdout)
                exe=self.work/(name+".native")
                clang=run([str(CLANG),"-Wno-override-module","-O2","-x","ir",
                           str(ir),"-o",str(exe)])
                self.assertEqual(clang.returncode,0,
                    "native clang/link failed "+name+": "+clang.stderr.decode(errors="replace")[-1800:])
                self.assertTrue(exe.is_file() and exe.stat().st_size>0,
                    "native artifact missing")
                native=run([str(exe)])
                self.assertEqual(native.returncode,expected,
                    "compiled native guest return mismatch "+name)
                print("ATTO_NATIVE_GUARD_VERIFIED="+name+" compiler="+self.compiler_digest+
                      " source="+hashlib.sha256(path.read_bytes()).hexdigest()+
                      " binary="+hashlib.sha256(exe.read_bytes()).hexdigest(),flush=True)
    def test_compiler_rejects_invalid_guard_before_any_ir_or_artifact(self):
        for name,source,diagnostic in NEGATIVE:
            with self.subTest(name=name):
                result,path=self.compile("reject_"+name,source)
                self.assertNotEqual(result.returncode,0,"compiler accepted "+name)
                self.assertNotIn(b"define ",result.stdout,"partial LLVM IR escaped for "+name)
                self.assertNotIn(b"target triple",result.stdout,"invalid IR header escaped for "+name)
                if diagnostic:
                    self.assertIn(diagnostic.lower(),result.stderr.decode(errors="replace").lower(),
                                  "unexpected compiler diagnostic for "+name)
                require_no_rejected_artifacts(path.parent, path, result.stdout)
                print("ATTO_NATIVE_GUARD_REJECTED="+name+" source="+
                       hashlib.sha256(path.read_bytes()).hexdigest(),flush=True)

if __name__=="__main__":
    unittest.main(argv=[__file__,*remaining],verbosity=2)
