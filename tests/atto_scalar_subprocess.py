"""Bounded, non-shell subprocess capture for native Atto compiler test harnesses."""
import os
import signal
import subprocess
import tempfile
import time


def run_bounded(argv, cwd, timeout=25, limit=4 * 1024 * 1024):
    if not isinstance(timeout, (int, float)) or not 0 < timeout <= 360:
        raise ValueError("invalid process timeout")
    if not isinstance(limit, int) or not 0 < limit <= 16 * 1024 * 1024:
        raise ValueError("invalid process output limit")
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        child = subprocess.Popen(argv, cwd=cwd, stdin=subprocess.DEVNULL,
                                 stdout=out, stderr=err, start_new_session=True)
        deadline = time.monotonic() + timeout
        try:
            while True:
                if os.fstat(out.fileno()).st_size > limit or os.fstat(err.fileno()).st_size > limit:
                    raise RuntimeError("native process exceeded bounded output")
                code = child.poll()
                if code is not None:
                    break
                if time.monotonic() >= deadline:
                    raise subprocess.TimeoutExpired(argv, timeout)
                time.sleep(0.02)
            if os.fstat(out.fileno()).st_size > limit or os.fstat(err.fileno()).st_size > limit:
                raise RuntimeError("native process exceeded bounded output on exit")
            out.seek(0)
            err.seek(0)
            return code, out.read(), err.read()
        except BaseException:
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            child.wait(timeout=5)
            raise
