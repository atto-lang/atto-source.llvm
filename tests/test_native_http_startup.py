import subprocess
import sys
import time
import unittest
from contextlib import contextmanager

from native_http_startup import wait_for_listener


@contextmanager
def child(code):
    process = subprocess.Popen([sys.executable, '-u', '-c', code], stdout=subprocess.PIPE, text=True)
    try:
        yield process
    finally:
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=3)


class StartupTests(unittest.TestCase):
    def test_valid_and_preserve_metrics(self):
        with child("print('LISTENING port=12345 workers=4'); print('{\"completed\": 1}')") as process:
            self.assertEqual(wait_for_listener(process), 12345)
            self.assertEqual(process.communicate(timeout=3)[0].strip(), '{"completed": 1}')

    def test_fragmented_line(self):
        with child("import time; print('LISTEN',end='',flush=True); time.sleep(.02); print('ING port=1')") as process:
            self.assertEqual(wait_for_listener(process), 1)

    def test_timeout(self):
        with child('import time; time.sleep(30)') as process:
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                wait_for_listener(process, .1)
            self.assertLess(time.monotonic() - start, 2)

    def test_partial_line_timeout(self):
        with child("import time; print('LISTENING port=123',end='',flush=True); time.sleep(30)") as process:
            with self.assertRaises(TimeoutError):
                wait_for_listener(process, .2)

    def test_early_exit(self):
        with child('pass') as process:
            with self.assertRaises(RuntimeError):
                wait_for_listener(process)

    def test_invalid_lines(self):
        for line in ('READY port=1', 'LISTENING', 'LISTENING port=0', 'LISTENING port=65536',
                     'LISTENING port=-1', 'LISTENING port=1 port=2', 'LISTENING port=1 bad',
                     'LISTENING port=abc', 'LISTENING port=１', 'x'*512):
            with self.subTest(line=line[:50]), child(f'print({line!r})') as process:
                with self.assertRaises(ValueError):
                    wait_for_listener(process)

    def test_invalid_timeout(self):
        with child('pass') as process:
            with self.assertRaises(ValueError):
                wait_for_listener(process, 0)


if __name__ == '__main__':
    unittest.main()
