#!/usr/bin/env python3
"""Check the proof translator's fail-closed boundary and SSA branch semantics."""
import importlib.util
import pathlib
import subprocess
import sys
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("state_smt", ROOT / "scripts/verify_state_scope_smt.py")
SMT = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SMT)
Z3 = sys.argv.pop(1) if len(sys.argv) > 1 else "z3"


def function(body, arguments="i8 %x"):
    return "define i32 @probe(" + arguments + ") {\nentry:\n" + body + "\n}\n"


class TranslatorTest(unittest.TestCase):
    def equivalent(self, ir, expected):
        _, declarations, expression, coverage = SMT.translate(ir, "probe")
        query = ("(set-logic QF_BV)\n" + declarations + "\n(assert (or (not " + coverage
                 + ") (not (= " + expression + " " + expected + "))))\n(check-sat)\n")
        result = subprocess.run([Z3, "-in"], input=query, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "unsat", query + result.stdout)

    def test_branch_phi(self):
        self.equivalent(function("""%c = icmp ult i8 %x, 128
br i1 %c, label %left, label %right
left:
%a = add i8 %x, 1
br label %join
right:
%b = sub i8 %x, 1
br label %join
join:
%v = phi i8 [ %a, %left ], [ %b, %right ]
%r = zext i8 %v to i32
ret i32 %r"""),
            "((_ zero_extend 24) (ite (bvult x (_ bv128 8)) (bvadd x (_ bv1 8)) (bvsub x (_ bv1 8))))")

    def test_switch_default_and_cases(self):
        self.equivalent(function("""switch i8 %x, label %other [
i8 0, label %zero
i8 -1, label %last
]
zero:
ret i32 1
last:
ret i32 2
other:
ret i32 3"""), "(ite (= x (_ bv0 8)) (_ bv1 32) (ite (= x (_ bv255 8)) (_ bv2 32) (_ bv3 32)))")

    def test_select_trunc_bit_operations(self):
        self.equivalent(function("""%a = and i8 %x, 15
%b = or i8 %a, 32
%c = xor i8 %b, 3
%d = trunc i8 %c to i1
%e = select i1 %d, i32 1, i32 0
ret i32 %e"""), "(ite (= (bvand x (_ bv1 8)) (_ bv0 8)) (_ bv1 32) (_ bv0 32))")

    def test_signed_compare_and_abort(self):
        self.equivalent(function("""%c = icmp slt i8 %x, 0
br i1 %c, label %bad, label %good
bad:
tail call void @abort() #1
unreachable
good:
ret i32 1"""), "(ite (bvslt x (_ bv0 8)) (_ bv0 32) (_ bv1 32))")

    def test_unsupported_is_rejected(self):
        for body in (
            "%a = load i32, ptr null\nret i32 %a",
            "%a = call i32 @unknown()\nret i32 %a",
            "%a = add nsw i8 %x, 1\nret i32 0",
            "%a = add nuw i8 %x, 1\nret i32 0",
            "%a = freeze i8 %x\nret i32 0",
            "ret i32 poison", "ret i32 undef", "unreachable",
            "br label %entry", "ret i32 0\nret i32 1",
            "%a = zext i8 %x to i32\n%a = add i32 %a, 1\nret i32 %a",
            "br label %join\njoin:\n%a = phi i32 [ 1, %missing ]\nret i32 %a",
            "switch i8 %x, label %other [\ni8 0, label %other\ni8 0, label %other\n]\nother:\nret i32 0",
            "%a = zext i32 %x to i64\nret i32 0",
            "call void @abort()\nret i32 0",
        ):
            with self.subTest(body=body), self.assertRaises((ValueError, KeyError)):
                SMT.translate(function(body), "probe")


if __name__ == "__main__":
    unittest.main()
