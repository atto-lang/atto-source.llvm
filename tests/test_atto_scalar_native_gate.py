#!/usr/bin/env python3
"""Source integrity, adverse mutation and host-only C observer checks.

Synthetic Clang LLVM IR witnesses are NOT AttoLang compiler-backed guests.
"""
import importlib.util
import shutil
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "atto_scalar_llvm_native", Path(__file__).with_name("atto_scalar_llvm_native.py"))
NATIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(NATIVE)


class CorpusAdversarialTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="atto-llvm-negative-source-")
        self.root = Path(self.tmp.name)
        source = ROOT / "contracts/atto-scalar-v1"
        shutil.copytree(source, self.root / "contracts/atto-scalar-v1")
    def tearDown(self):
        self.tmp.cleanup()
    def test_canonical_inventory(self):
        self.assertEqual(len(NATIVE.load_corpus(self.root)), 19)
    def test_modified_positive_source_rejected(self):
        target = self.root / "contracts/atto-scalar-v1/positive/canonical-add.atto"
        target.write_bytes(target.read_bytes().replace(b"40 + 2", b"40 + 3"))
        with self.assertRaisesRegex(RuntimeError, "mismatched scalar fixture"):
            NATIVE.load_corpus(self.root)
    def test_modified_negative_source_rejected(self):
        target = self.root / "contracts/atto-scalar-v1/negative/removed-caret.atto"
        target.write_bytes(target.read_bytes().replace(b"2 ^ 3", b"2 ^^ 3"))
        with self.assertRaisesRegex(RuntimeError, "mismatched scalar fixture"):
            NATIVE.load_corpus(self.root)
    def test_manifest_evidence_promotion_rejected(self):
        target = self.root / "contracts/atto-scalar-v1/cases.json"
        target.write_bytes(target.read_bytes().replace(
            b'"evidence_status": "NOT_EXECUTED"', b'"evidence_status": "EXECUTED"'))
        with self.assertRaisesRegex(RuntimeError, "unreviewed Atto scalar manifest"):
            NATIVE.load_corpus(self.root)
    def test_symlink_source_rejected(self):
        target = self.root / "contracts/atto-scalar-v1/positive/canonical-add.atto"
        original = target.with_suffix(".original")
        target.rename(original)
        try:
            target.symlink_to(original)
        except (OSError, NotImplementedError):
            self.skipTest("symlink creation prohibited")
        with self.assertRaisesRegex(RuntimeError, "symlinked source"):
            NATIVE.load_corpus(self.root)
    def test_empty_ir_cannot_impersonate_native_guest(self):
        with self.assertRaisesRegex(RuntimeError, "expected source-owned"):
            NATIVE.native_ir(b"; ModuleID = 'forged'\n")
    def test_host_only_full_i64_observer_o0_o2(self):
        # These are synthetic host LLVM IR witnesses, NOT Atto compiler
        # execution. They prove that the observer can preserve the full
        # signed 64-bit value at both optimization levels.
        clang = shutil.which("clang")
        if clang is None:
            self.skipTest("host Clang not installed")
        for value in (-9223372036854775808, 9223372036854775806):
            for optimization in NATIVE.OPTIMIZATIONS:
                with self.subTest(value=value, optimization=optimization):
                    ir = (
                        "; ModuleID = 'host-only-typed-i64-observer'\\n"
                        "define i64 @__oreslang_guest_main() {\\n"
                        "entry:\\n"
                        f"  ret i64 {value}\\n"
                        "}\\n"
                        "define i32 @main() {\\n"
                        "entry:\\n"
                        "  ret i32 0\\n"
                        "}\\n"
                    ).encode("ascii")
                    with tempfile.TemporaryDirectory(prefix="atto-host-i64-") as tmp:
                        work = Path(tmp)
                        native = NATIVE.link_native(Path(clang), ir, work, optimization)
                        rc, output, diagnostic = NATIVE.execute([str(native)], work, 10)
                        self.assertEqual((rc, output, diagnostic),
                                         (0, (str(value) + "\\n").encode("ascii"), b""))

    def test_duplicate_or_referenced_os_main_denied(self):
        raw = (b"define i64 @__oreslang_guest_main() { ret i64 42 }\n"
               b"define i32 @main() { ret i32 42 }\n")
        self.assertIn(b"@__atto_observed_os_wrapper", NATIVE.native_ir(raw))
        with self.assertRaisesRegex(RuntimeError, "unexpected LLVM references"):
            NATIVE.native_ir(raw + b"declare i32 @main()\n")


if __name__ == "__main__":
    unittest.main()
