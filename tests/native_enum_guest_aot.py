#!/usr/bin/env python3
"""Exact compiler-source native enum AOT smoke. Requires built oreslang-llvmc and Clang.
No interpreter, synthetic model, or cross-repository private source transfer.
"""
import argparse
import pathlib
import re
import subprocess
import tempfile
import unittest


class NativeEnumGuestAot(unittest.TestCase):
    compiler = ""
    clang = ""
    fixture = ""

    def compile(self, text, *extra):
        with tempfile.TemporaryDirectory(prefix="ores-enum-guest-aot-") as temp:
            src = pathlib.Path(temp) / "guest.ores"
            src.write_text(text, encoding="utf-8")
            return subprocess.run(
                [self.compiler, "--experimental-enum-aot", *extra, str(src)],
                text=True, capture_output=True, timeout=20, check=False)

    def test_native_guest_binary_and_static_symbols(self):
        text = pathlib.Path(self.fixture).read_text(encoding="utf-8")
        result = self.compile(text)
        self.assertEqual(result.returncode, 0, result.stderr)
        ir = result.stdout
        self.assertIn("define i32 @main()", ir)
        exports = re.findall(r'define i64 @("?eos2[^\s(]+"?)\(', ir)
        calls = re.findall(r'call i64 @("?eos2[^\s(]+"?)\(', ir)
        self.assertEqual(len(exports), 3, ir)
        self.assertEqual(len(calls), 2, ir)
        self.assertNotEqual(calls[0], calls[1], ir)
        self.assertTrue(set(calls) <= set(exports))
        self.assertNotIn("switch ", ir)
        self.assertNotIn("oreslang_interpreter", ir)

        with tempfile.TemporaryDirectory(prefix="ores-enum-bin-") as temp:
            ll = pathlib.Path(temp) / "guest.ll"
            out = pathlib.Path(temp) / "guest"
            ll.write_text(ir, encoding="utf-8")
            link = subprocess.run([self.clang, "-x", "ir", "-O0",
                                   str(ll), "-o", str(out)],
                                  capture_output=True, text=True, timeout=30,
                                  check=False)
            self.assertEqual(link.returncode, 0, link.stderr)
            native = subprocess.run([str(out)], capture_output=True,
                                    text=True, timeout=10, check=False)
            self.assertEqual(native.returncode, 10, native.stderr)

    def test_without_experimental_flag_remains_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ores-enum-gate-") as temp:
            src = pathlib.Path(temp) / "guest.ores"
            src.write_text(pathlib.Path(self.fixture).read_text(encoding="utf-8"))
            process = subprocess.run([self.compiler, str(src)], capture_output=True,
                                     text=True, timeout=20, check=False)
            self.assertNotEqual(process.returncode, 0)
            self.assertFalse(process.stdout)

    def test_widened_parent_without_fallback_is_rejected(self):
        text = """define enum Foo as A, B end
            fnc foo(Foo.A x): int { return 10; }
            fnc foo(Foo.B x): int { return 20; }
            fnc main(): int {
                const Foo widened = Foo.A;
                return foo(widened);
            }"""
        result = self.compile(text)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(result.stdout)

    def test_fail_closed_invalid_guest_forms(self):
        base = """define enum Foo as A, B end
            fnc foo(Foo.A x): int { return 10; }
            fnc foo(Foo x): int { return 0; }
            fnc main(): int { %s }"""
        cases = [
            base % "const Foo.A bad = Foo.B; return foo(bad);",
            base % "return foo(Foo.Missing);",
            base % "return foo(Bar.A);",
            base % "return foo(unknown);",
            base % "const Foo v = Foo.A; return foo(Foo.Missing);",
            base % "let Foo v = Foo.A; return foo(v);",
            base % "return foo(foo(Foo.A));",
            base % "return foo(Foo.A) + 1;",
            base % "const Foo foo = Foo.A; return foo(foo);",
            base % "return foo(Foo.A); return foo(Foo.A);",
            base.replace("A, B", "A, A") % "return foo(Foo.A);",
            base.replace("A, B", "A = 3, B = 2") % "return foo(Foo.A);",
            base.replace("A, B", "A = 0, B = 1") % "return foo(Foo.A);",
            base.replace("A, B", "A = 2147483647, B") % "return foo(Foo.A);",
            base.replace("Foo.A x", "Foo.A x, Foo.B y") % "return foo(Foo.A);",
            base.replace("return 10", "return foo(Foo.A)") % "return foo(Foo.A);",
            base.replace("fnc foo(Foo.A x)", "async fnc foo(Foo.A x)") % "return foo(Foo.A);",
            base.replace("fnc foo(Foo x)", "fnc foo(Foo.A x)") % "return foo(Foo.A);",
            base.replace("A, B", "A: int, B") % "return foo(Foo.A);",
            "import fnc foo from \"unknown.ores\";\n" + base % "return foo(Foo.A);",
            base % "const Foo v = Foo.A; const Foo v = Foo.B; return foo(v);",
        ]
        for index, text in enumerate(cases):
            with self.subTest(case=index):
                result = self.compile(text)
                self.assertNotEqual(result.returncode, 0, text)
                self.assertFalse(result.stdout, text)
                self.assertIn("native enum AOT", result.stderr, result.stderr)

    def test_cross_target_and_other_experiments_rejected(self):
        text = pathlib.Path(self.fixture).read_text(encoding="utf-8")
        for flags in [("--target=wasm32",),
                      ("--target=thumbv7m-none-eabi",),
                      ("--experimental-owned-int",),
                      ("--experimental-rt-scalar-loop",)]:
            with self.subTest(flags=flags):
                result = self.compile(text, *flags)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(result.stdout)


if __name__ == "__main__":
    args = argparse.ArgumentParser()
    args.add_argument("--compiler", required=True)
    args.add_argument("--clang", required=True)
    args.add_argument("--fixture", required=True)
    opts = args.parse_args()
    NativeEnumGuestAot.compiler = opts.compiler
    NativeEnumGuestAot.clang = opts.clang
    NativeEnumGuestAot.fixture = opts.fixture
    unittest.main(argv=[__file__], verbosity=2)
