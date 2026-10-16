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


if __name__ == "__main__":
    unittest.main()
