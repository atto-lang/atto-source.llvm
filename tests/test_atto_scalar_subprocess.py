"""Adversarial bounded-process tests: fake tools, no compiler proof."""
import subprocess
import sys
import time
import tempfile
from pathlib import Path
import unittest

from atto_scalar_subprocess import run_bounded


class BoundedScalarProcessTests(unittest.TestCase):
    def test_success(self):
        with tempfile.TemporaryDirectory() as directory:
            observed = run_bounded(
                [sys.executable, "-c", "print('hello')"], Path(directory))
            self.assertEqual(observed, (0, b"hello\n", b""))

    def test_rejected_exit_diagnostic_preserved(self):
        with tempfile.TemporaryDirectory() as directory:
            observed = run_bounded(
                [sys.executable, "-c",
                 "import sys;sys.stderr.write('reject');sys.exit(7)"],
                Path(directory))
            self.assertEqual(observed, (7, b"", b"reject"))

    def test_stdout_bomb_denied(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "bounded output"):
                run_bounded(
                    [sys.executable, "-c",
                     "import sys;sys.stdout.buffer.write(b'X'*2000000)"],
                    Path(directory), limit=16*1024)

    def test_stderr_bomb_denied(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(RuntimeError, "bounded output"):
                run_bounded(
                    [sys.executable, "-c",
                     "import sys;sys.stderr.buffer.write(b'X'*2000000)"],
                    Path(directory), limit=16*1024)

    def test_timeout_kills_child(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaises(subprocess.TimeoutExpired):
                run_bounded(
                    [sys.executable, "-c", "import time;time.sleep(5)"],
                    Path(directory), timeout=0.1)

    def test_clean_parent_cannot_leave_a_background_child(self):
        # Executable guest, wrapper or linker must not outlive a successful
        # native test by leaving work behind in the process group.
        with tempfile.TemporaryDirectory() as directory:
            code = (
                "import subprocess,sys\n"
                "subprocess.Popen([sys.executable,'-c',"
                "\"import time,pathlib;time.sleep(.5);"
                "pathlib.Path('escaped').write_text('survived')\""
                "],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)\n"
                "print('parent-done')\n"
            )
            result = run_bounded([sys.executable, "-c", code], Path(directory))
            self.assertEqual(result, (0, b"parent-done\n", b""))
            time.sleep(.7)
            self.assertFalse((Path(directory) / "escaped").exists())

    def test_timeout_cannot_leave_a_background_child(self):
        with tempfile.TemporaryDirectory() as directory:
            code = (
                "import subprocess,sys,time\n"
                "subprocess.Popen([sys.executable,'-c',"
                "\"import time,pathlib;time.sleep(.5);"
                "pathlib.Path('escaped').write_text('survived')\""
                "],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)\n"
                "time.sleep(5)\n"
            )
            with self.assertRaises(subprocess.TimeoutExpired):
                run_bounded([sys.executable, "-c", code],
                            Path(directory), timeout=.15)
            time.sleep(.7)
            self.assertFalse((Path(directory) / "escaped").exists())

    def test_invalid_limits_fail_before_launch(self):
        with tempfile.TemporaryDirectory() as directory:
            for limit in (0, -1, 32*1024*1024, 1.3):
                with self.subTest(limit=limit):
                    with self.assertRaises(ValueError):
                        run_bounded([sys.executable, "-c", "pass"],
                                    Path(directory), limit=limit)


if __name__ == "__main__":
    unittest.main()
