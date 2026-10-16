"""Synthetic IR tests for the native guest-entry test harness, not compiler parity."""
import importlib.util
from pathlib import Path
import unittest

TARGET = Path(__file__).with_name("native_numeric_portable_i64_source.py")
SPEC = importlib.util.spec_from_file_location("llvm_numeric_harness", TARGET)
H = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(H)


class NativeNumericHarnessSelfTest(unittest.TestCase):
    def test_native_wrapper_is_rewritten_once_and_guest_i64_preserved(self):
        ir = ('; @main() in a comment must not be modified\n'
              'define i64 @__oreslang_guest_main() { ret i64 42 }\n'
              'define i32 @main() { ret i32 0 }\n')
        self.assertIsNotNone(H.ENTRY.search(ir))
        rewritten, count = H.HOST_WRAPPER.subn(r"\1@oreslang_os_main", ir)
        self.assertEqual(count, 1)
        self.assertIn("define i64 @__oreslang_guest_main()", rewritten)
        self.assertIn("define i32 @oreslang_os_main()", rewritten)
        self.assertIn("; @main() in a comment", rewritten)

    def test_missing_or_duplicated_native_wrappers_cannot_count_as_execution(self):
        for ir, expected in (
            ("; define i32 @main()\n", 0),
            ("define i64 @main() { ret i64 1 }\n", 0),
            ("define i32 @main() {}\ndefine i32 @main() {}\n", 2),
        ):
            with self.subTest(ir=ir):
                self.assertEqual(len(H.HOST_WRAPPER.findall(ir)), expected)

    def test_comment_is_not_guest_entry(self):
        bogus = "; define i64 @__oreslang_guest_main()\n"
        self.assertIsNone(H.ENTRY.search(bogus))

    def test_checked_overflow_trap_must_follow_exact_intrinsic_flag(self):
        # Synthetic IR only: verifies the harness cannot count a generic
        # process signal without a checked overflow guard in compiler IR.
        valid = (
            'target triple = "x86_64-pc-linux-gnu"\n'
            'declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64,i64)\n'
            'declare void @llvm.trap()\n'
            'define i64 @__oreslang_guest_main() {\n'
            'entry:\n'
            '  %i64.checked.pair = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 1,i64 2)\n'
            '  %i64.checked.value = extractvalue { i64, i1 } %i64.checked.pair, 0\n'
            '  %i64.checked.overflow = extractvalue { i64, i1 } %i64.checked.pair, 1\n'
            '  br i1 %i64.checked.overflow, label %i64.overflow.trap, label %i64.checked.continue\n'
            'i64.overflow.trap: ; preds = %entry\n'
            '  call void @llvm.trap()\n'
            '  unreachable\n'
            'i64.checked.continue: ; preds = %entry\n'
            '  ret i64 %i64.checked.value\n'
            '}\n')
        H.assert_checked_overflow_path(valid, "sadd")
        decoy = valid.replace(
            "define i64 @__oreslang_guest_main() {",
            "define i64 @decoy() {") + (
                "define i64 @__oreslang_guest_main() {\n"
                "entry:\n"
                "  ret i64 42\n}\n")
        with self.assertRaisesRegex(AssertionError, "intrinsic absent"):
            H.assert_checked_overflow_path(decoy, "sadd")
        with self.assertRaisesRegex(AssertionError, "exactly one guest main"):
            H.assert_checked_overflow_path(valid + valid, "sadd")
        mutants = (
            (valid, "smul"),
            (valid.replace("call void @llvm.trap()", "call void @other()"), "sadd"),
            (valid.replace("br i1 %i64.checked.overflow",
                           "br i1 %unrelated.condition"), "sadd"),
            (valid.replace("  br i1 %i64.checked.overflow",
                           "  ; br i1 %i64.checked.overflow"), "sadd"),
            (valid.replace("  br i1 %i64.checked.overflow",
                           "  %unused = call { i64, i1 } @llvm.smul.with.overflow.i64(i64 2,i64 3)\n"
                           "  br i1 %i64.checked.overflow"), "smul"),
            (valid.replace("extractvalue { i64, i1 } %i64.checked.pair, 1",
                           "extractvalue { i64, i1 } %unrelated.pair, 1"), "sadd"),
            (valid.replace("  ret i64 %i64.checked.value",
                           "  call void @llvm.trap()\n  unreachable\n"
                           "  ret i64 %i64.checked.value"), "sadd")
        )
        for mutant, operation in mutants:
            with self.subTest(operation=operation, excerpt=mutant[-50:]):
                with self.assertRaises(AssertionError):
                    H.assert_checked_overflow_path(mutant, operation)



if __name__ == "__main__":
    unittest.main()
