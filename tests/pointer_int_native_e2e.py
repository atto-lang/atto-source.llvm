#!/usr/bin/env python3
"""Actual opt-in Atto Pointer<int> source -> LLVM IR -> Clang -> native executable.

The admitted M1 specialization is one synchronous, zero-escape, stack-promoted
region of 1..16 int slots. This is NOT generic Pointer<T>, heap allocation,
actor ownership transfer, or checked dynamic index support.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

argp = argparse.ArgumentParser()
argp.add_argument("--compiler", required=True)
argp.add_argument("--clang", required=True)
args, unknown = argp.parse_known_args()
COMPILER = Path(args.compiler).resolve()
CLANG = Path(args.clang).resolve()
assert COMPILER.is_file() and os.access(COMPILER, os.X_OK), "REAL_LLVM_COMPILER_REQUIRED"
assert CLANG.is_file() and os.access(CLANG, os.X_OK), "REAL_NATIVE_CLANG_REQUIRED"


def program(statements, capacity=4, outcome="first + second"):
    return (
        "pub fnc main(): int {\n"
        + f"  let mut Pointer<int> p = Pointer.alloc<int>({capacity});\n"
        + statements
        + "\n  return " + outcome + ";\n}\n"
    )


POSITIVE = [
    ("pointer_sum_142", program(
        "  p.write_at(0, 42);\n  p.write_at(1, 100);\n"
        "  val int first = p.read_at(0);\n"
        "  val int second = p.read_at(1);\n  p.close();"), 142),
    ("pointer_inplace_value_6", program(
        "  p.write_at(0, 2);\n  p.write_at(0, p.read_at(0) * 3);\n"
        "  val int first = p.read_at(0);\n  p.close();", outcome="first"), 6),
    ("pointer_nested_22", program(
        "  p.write_at(0, 7);\n"
        "  p.write_at(1, (p.read_at(0) * 3) + 1);\n"
        "  val int first = p.read_at(1);\n  p.close();", outcome="first"), 22),
    ("pointer_last_index_255", program(
        "  p.write_at(15, 255);\n"
        "  val int first = p.read_at(15);\n  p.close();",
        capacity=16, outcome="first"), 255),
    ("pointer_drop_without_reads", program(
        "  p.close();", capacity=1, outcome="0"), 0),
    ("pointer_comment_is_inert", program(
        "  /* p.close(); /* Pointer<string> */ &p; */\n"
        "  p.write_at(0, 41); // no use of * or & for pointer manipulation\n"
        "  val int first = p.read_at(0);\n  p.close();",
        outcome="first + 1"), 42),
]
NEGATIVE = [
    ("uninitialized_read", program(
        "  val int first = p.read_at(0);\n  p.close();", outcome="first"), "uninitialized"),
    ("read_uninitialized_before_first_write", program(
        "  p.write_at(0, p.read_at(0));\n  p.close();", outcome="0"), "uninitialized"),
    ("out_of_bounds", program("  p.write_at(4, 1);\n  p.close();", outcome="0"), "bounds"),
    ("negative_index", program("  p.write_at(-1, 1);\n  p.close();", outcome="0"), "nonnegative"),
    ("allocation_zero", program("  p.close();", capacity=0, outcome="0"), "bounds"),
    ("allocation_too_large", program("  p.close();", capacity=17, outcome="0"), "bounds"),
    ("double_close", program("  p.close();\n  p.close();", outcome="0"), "double close"),
    ("missing_close", program("  p.write_at(0, 1);", outcome="0"), "close exactly once"),
    ("read_after_close", program("  p.close();", outcome="p.read_at(0)"), "read after close"),
    ("write_after_close", program("  p.close();\n  p.write_at(0, 1);", outcome="0"), "write after close"),
    ("pointer_escape", program("  val int first = p;\n  p.close();", outcome="first"), "escape"),
    ("second_pointer", program(
        "  let mut Pointer<int> q = Pointer.alloc<int>(1);\n  p.close();",
        outcome="0"), "expected 'p'"),
    ("int_overflow", program("  p.close();", outcome="9223372036854775807 + 1"), "overflow"),
    ("unsupported_pointer_type", program(
        "  p.close();", outcome="0").replace("Pointer<int> p", "Pointer<string> p"), "expected 'int'"),
    ("unsupported_ffi", program(
        "  printf(1);\n  p.close();", outcome="0"), "expected 'p'"),
    ("unary_address", program(
        "  p.write_at(0, 1);\n  val int first = &p;\n  p.close();", outcome="first"), "unsupported token"),
    ("pointer_return", program("  p.close();", outcome="p"), "escape"),
]


def run(cmd, env=None):
    return subprocess.run(cmd, capture_output=True, timeout=120, env=env)


class RealPointerNativeAot(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.TemporaryDirectory(prefix="atto-pointer-aot-")
        cls.dir = Path(cls.workdir.name)
        cls.compiler_sha = hashlib.sha256(COMPILER.read_bytes()).hexdigest()
        print("ATTO_POINTER_INT_COMPILER_SHA256=" + cls.compiler_sha, flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.workdir.cleanup()

    def compile(self, name, source, admitted=True):
        self.assertLess(len(source.encode("utf-8")), 4096)
        path = self.dir / (name + ".atto")
        path.write_text(source, encoding="utf-8")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        cmd = [str(COMPILER), "--target=host"]
        if admitted:
            cmd.append("--experimental-pointer-int-aot")
        cmd.append(str(path))
        result = run(cmd)
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), digest,
                         "compiler mutated its source input")
        return result, digest

    def test_positive_pointer_int_aot_executes(self):
        for name, source, expected in POSITIVE:
            with self.subTest(name=name):
                out, digest = self.compile(name, source)
                self.assertEqual(out.returncode, 0, out.stderr.decode(errors="replace")[-2000:])
                ir = out.stdout
                for needle in (b"define i32 @main()", b"alloca", b"getelementptr",
                               b"store volatile i64 0"):
                    self.assertIn(needle, ir, "missing real LLVM memory operation")
                self.assertNotIn(b"declare ptr @malloc", ir)
                text_ir = self.dir / (name + ".ll")
                text_ir.write_bytes(ir)
                executable = self.dir / (name + ".native")
                compiled = run([str(CLANG), "-Wno-override-module", "-O2",
                                "-x", "ir", str(text_ir), "-o", str(executable)])
                self.assertEqual(compiled.returncode, 0,
                                 compiled.stderr.decode(errors="replace")[-1600:])
                self.assertTrue(executable.is_file() and executable.stat().st_size > 0)
                executed = run([str(executable)])
                self.assertEqual(executed.returncode, expected,
                                 name + ": compiled native guest exit differs")
                # This is the guest binary (not merely the source-free
                # C++ capability model) running under address/UB sanitizers.
                sanitized = self.dir / (name + ".asan.native")
                san_build = run([str(CLANG), "-Wno-override-module", "-O1",
                                 "-fsanitize=address,undefined",
                                 "-fno-omit-frame-pointer",
                                 "-x", "ir", str(text_ir), "-o", str(sanitized)])
                self.assertEqual(san_build.returncode, 0,
                    "compiled Pointer<int> sanitizer build failed " + name +
                    ": " + san_build.stderr.decode(errors="replace")[-1600:])
                env = os.environ.copy()
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
                sanitized_run = run([str(sanitized)], env=env)
                self.assertEqual(sanitized_run.returncode, expected,
                    "compiled Pointer<int> sanitizer guest failed " + name +
                    ": " + sanitized_run.stderr.decode(errors="replace")[-1600:])
                print("ATTO_POINTER_INT_NATIVE_PASS=" + name + " source=" + digest +
                      " executable=" + hashlib.sha256(executable.read_bytes()).hexdigest(),
                      flush=True)

    def test_pointer_program_rejects_without_ir_or_artifact(self):
        for name, source, expected in NEGATIVE:
            with self.subTest(name=name):
                result, digest = self.compile("reject_" + name, source)
                self.assertNotEqual(result.returncode, 0, "accepted unsafe " + name)
                self.assertEqual(result.stdout, b"", "rejected source leaked LLVM IR")
                self.assertIn(expected.lower(), result.stderr.decode(errors="replace").lower())
                self.assertFalse((self.dir / ("reject_" + name + ".native")).exists())
                print("ATTO_POINTER_INT_REJECT=" + name + " source=" + digest, flush=True)

    def test_opt_in_is_required_and_target_scope_is_closed(self):
        source = POSITIVE[0][1]
        no_flag, _ = self.compile("no_pointer_flag", source, admitted=False)
        self.assertNotEqual(no_flag.returncode, 0)
        self.assertEqual(no_flag.stdout, b"")
        file = self.dir / "no_pointer_flag.atto"
        unsupported = run([str(COMPILER), "--experimental-pointer-int-aot",
                           "--target=wasm32", str(file)])
        self.assertNotEqual(unsupported.returncode, 0)
        self.assertEqual(unsupported.stdout, b"")


if __name__ == "__main__":
    unittest.main(argv=[__file__, *unknown], verbosity=2)
