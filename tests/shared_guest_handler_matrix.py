#!/usr/bin/env python3
"""Real native LLVM execution of the shared Java/Graal guest handler matrix.

This is an executable native guest-function test. It is not a guest socket
listener, native ActorGroup scheduler test, or Native Image benchmark.
"""
import argparse
import ctypes
import hashlib
import json
import pathlib
import subprocess
import tempfile

EXPECTED_BLOB = "a0b3fc658d1460eab62c85b3cc897143ea77a7ff"


def sha_blob(data):
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\x00" + data).hexdigest()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    p.add_argument("--fixture", type=pathlib.Path, required=True)
    args = p.parse_args()

    source = args.fixture.read_bytes()
    if sha_blob(source) != EXPECTED_BLOB:
        raise RuntimeError("shared guest handler source drifted from Java/Graal")
    compiled = subprocess.run([args.compiler, str(args.fixture)],
                              capture_output=True, text=True, check=True, timeout=30)
    ir = compiled.stdout
    if "define i64 @counter_response" not in ir:
        raise RuntimeError("public guest counter_response not compiled into LLVM")
    if "gc.statepoint" in ir or "gc.relocate" in ir:
        raise RuntimeError("unexpected tracing-GC calls in native guest handler")
    with tempfile.TemporaryDirectory(prefix="oreslang-guest-matrix-") as directory:
        library = pathlib.Path(directory) / "guest.so"
        result = subprocess.run(
            [args.clang, "-x", "ir", "-shared", "-fPIC", "-", "-o", str(library)],
            input=ir, capture_output=True, text=True, timeout=45)
        if result.returncode != 0:
            raise RuntimeError("Clang native guest compilation failed: " + result.stderr)
        native = ctypes.CDLL(str(library))
        counter = native.counter_response
        counter.argtypes = [ctypes.c_int64]
        counter.restype = ctypes.c_int64
        # Run the same mandatory edge probes as the shared semantics gate.
        # Execute the actual public main function rather than deriving its
        # expected result from counter_response.
        entrypoint = native.main
        entrypoint.argtypes = []
        entrypoint.restype = ctypes.c_int64
        main_result = entrypoint()
        if main_result != 43:
            raise AssertionError(f"native guest main() = {main_result}, expected 43")
        observations = {"main()": main_result}
        for n in (-1, 0, *range(1, 129)):
            actual = counter(n)
            expected = 43 * n
            if actual != expected:
                raise AssertionError(f"native guest counter_response({n}) = {actual}, expected {expected}")
            observations[f"counter_response({n})"] = actual
        print(f"LLVM_GUEST_SHARED_HANDLER_PASS cases={len(observations)} fixture_blob={EXPECTED_BLOB}")
        print(json.dumps({"backend": "llvm", "observed": observations}, sort_keys=True))


if __name__ == "__main__":
    main()
