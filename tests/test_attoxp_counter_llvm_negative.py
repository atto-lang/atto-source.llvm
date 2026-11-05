#!/usr/bin/env python3
"""Host-only regression of bounded native compiler negative capture (not guest proof)."""
import importlib.util
import pathlib
import tempfile
import unittest

MODULE = pathlib.Path(__file__).with_name("attoxp_counter_llvm_negative.py")
spec = importlib.util.spec_from_file_location("attoxp_neg", MODULE)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class NegativeCompilerCapture(unittest.TestCase):
    def compiler(self, directory, script):
        path = pathlib.Path(directory) / "fake-compiler"
        path.write_text("#!/usr/bin/env python3\n" + script)
        path.chmod(0o700)
        return path

    def test_bounded_expected_rejection(self):
        with tempfile.TemporaryDirectory() as directory:
            compiler = self.compiler(directory, "import sys;sys.stderr.write('expected rejected syntax\\n');sys.exit(1)\n")
            self.assertEqual(gate.run_bounded(compiler, pathlib.Path(directory)), (1, b"", b"expected rejected syntax\n"))

    def test_stdout_and_stderr_floods_are_rejected_before_full_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            for stream in (1, 2):
                with self.subTest(stream=stream):
                    compiler = self.compiler(directory, "import os;os.write("+str(stream)+",b'x'*131072)\n")
                    with self.assertRaisesRegex(RuntimeError, "output budget"):
                        gate.run_bounded(compiler, pathlib.Path(directory), max_bytes=4096)

    def test_timeout_terminates_compiler(self):
        with tempfile.TemporaryDirectory() as directory:
            compiler = self.compiler(directory, "import time;time.sleep(3)\n")
            with self.assertRaisesRegex(RuntimeError, "timeout"):
                gate.run_bounded(compiler, pathlib.Path(directory), timeout=0.1)

    def test_invalid_budgets_prevent_spawn(self):
        with tempfile.TemporaryDirectory() as directory:
            for options in ({"timeout":0}, {"max_bytes":0}):
                with self.assertRaises(ValueError):
                    gate.run_bounded(pathlib.Path(directory)/"missing", pathlib.Path(directory), **options)


if __name__ == "__main__":
    unittest.main()
