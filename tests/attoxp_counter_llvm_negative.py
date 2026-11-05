#!/usr/bin/env python3
"""Native compiler negative admission for independently authored AttoXP slices.

This is a real compiler invocation. No successful IR/stdout is allowed.
"""
import argparse
import os
import pathlib
import selectors
import signal
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
NEGATIVE = ("unsupported-op.atto", "unknown-call.atto", "bad-arity.atto")

def run_bounded(compiler, path, timeout=8.0, max_bytes=65536):
    """Bound stdout+stderr *during* capture; kill the entire compiler group on failure."""
    if timeout <= 0 or max_bytes <= 0:
        raise ValueError("invalid child process budget")
    process = subprocess.Popen(
        [str(compiler), str(path)],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        start_new_session=True,
    )
    streams = selectors.DefaultSelector()
    buffers = {"stdout": bytearray(), "stderr": bytearray()}
    completed = False
    deadline = time.monotonic() + timeout
    try:
        for key, stream in (("stdout", process.stdout), ("stderr", process.stderr)):
            os.set_blocking(stream.fileno(), False)
            streams.register(stream, selectors.EVENT_READ, key)
        while streams.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("compiler negative probe timeout")
            ready = streams.select(remaining)
            if not ready:
                raise RuntimeError("compiler negative probe timeout")
            for event, _ in ready:
                stream = event.fileobj
                label = event.data
                budget_left = max_bytes - sum(map(len, buffers.values()))
                try:
                    data = os.read(stream.fileno(), min(8192, budget_left + 1))
                except BlockingIOError:
                    continue
                if not data:
                    streams.unregister(stream)
                    continue
                buffers[label].extend(data)
                if sum(map(len, buffers.values())) > max_bytes:
                    raise RuntimeError("compiler negative probe output budget exceeded")
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise RuntimeError("compiler negative probe timeout")
        code = process.wait(timeout=remaining)
        completed = True
        return code, bytes(buffers["stdout"]), bytes(buffers["stderr"])
    finally:
        if not completed:
            try:
                os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            process.wait()
        streams.close()
        process.stdout.close()
        process.stderr.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    if not compiler.is_file() or args.compiler.is_symlink():
        raise SystemExit("missing or symlinked native compiler")
    for name in NEGATIVE:
        path = ROOT / "contracts" / "attoxp-counter-v1" / name
        if path.is_symlink() or not path.is_file():
            raise SystemExit("missing or linked fixture " + name)
        code, stdout, stderr = run_bounded(compiler, path)
        if not (code > 0 and stdout == b"" and 0 < len(stderr) <= 65536):
            raise SystemExit("LLVM compiler unexpectedly admitted " + name)
    print("ATTOXP_LLVM_COUNTER_NEGATIVE:3")

if __name__ == "__main__":
    main()
