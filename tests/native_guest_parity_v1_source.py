#!/usr/bin/env python3
"""Exact-source Oreslang -> LLVM IR -> Clang -> native guest processes.

Pinned common-source PR #92 fixture bytes; no cross-repository token or network.
A passing scalar/class slice does NOT prove generic ownership or zero tracing GC.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FIXTURES = {
    "scalar-main": (
        """pub fnc main(): int {
  return 20 + 22;
}
""",
        "ab1ac790ba3928b8876b428230ae4215984380a221c02c604f85cd3fca1d790f",
        "return",
    ),
    "owned-array-main": (
        """define class Shelf as
  pub let mut Array<int> items = new Array<int>();
end
pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  shelf.items.add(21);
  shelf.items.set(0, 42);
  return shelf.items.get(0);
}
""",
        "1c907b5f62497f984e775fc928284de16238aa32a719c161090b50b4cd030370",
        "return",
    ),
    "private-array-negative": (
        """define class Shelf as
  let mut Array<int> hidden = new Array<int>();
end
pub fnc main(): int {
  let mut Shelf shelf = new Shelf();
  return shelf.hidden.size;
}
""",
        "c08759e2a948b32dedb488ca15653460f7b1844f8bb02cb6a71297e00a6519f5",
        "reject",
    ),
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def run(argv, *, timeout=90, env=None):
    try:
        result = subprocess.run(argv, input=b"", capture_output=True,
                                timeout=timeout, check=False, env=env)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError("process timed out: " + Path(argv[0]).name) from error
    if len(result.stdout) > 131072 or len(result.stderr) > 131072:
        raise RuntimeError("compiler or linked guest exceeded 128 KiB output cap")
    return result


def verify(compiler, clang, source_root, sanitizers):
    runtime = source_root / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = source_root / "src/main/cpp"
    for binary in (compiler, clang, runtime):
        if not binary.is_file():
            raise RuntimeError("missing compiler/clang/runtime: " + str(binary))
    with tempfile.TemporaryDirectory(prefix="ores-guest-v1-") as td:
        tmp = Path(td)
        for name, (source, expected_hash, kind) in FIXTURES.items():
            exact = source.encode("utf-8")
            if sha256(exact) != expected_hash:
                raise RuntimeError(name + ": shared contract fixture differs")
            fixture = tmp / (name + ".ores")
            fixture.write_bytes(exact)
            compiled = run([str(compiler), str(fixture)])
            if kind == "reject":
                if (compiled.returncode != 1 or compiled.stdout or
                        b"private class field" not in compiled.stderr):
                    raise RuntimeError(name + ": expected precise no-IR compiler rejection")
                print("PASS: " + name + ": diagnostic and zero generated IR", flush=True)
                continue
            ir = compiled.stdout
            if compiled.returncode != 0 or compiled.stderr or b"define " not in ir:
                raise RuntimeError(name + ": actual native compiler failed to emit IR")
            if name == "owned-array-main":
                required = (b"@oreslang_int_array_try_new", b"@oreslang_int_array_drop",
                            b"@oreslang_int_array_region_close",
                            b"@oreslang_int_array_try_region_open",
                            b"array.resource.failure:")
                if any(token not in ir for token in required):
                    raise RuntimeError(name + ": missing native allocation/drop guards")
            ir_file = tmp / (name + ".ll")
            ir_file.write_bytes(ir)
            guest = tmp / name
            command = [str(clang), "-O1", "-Wno-override-module", "-std=c11",
                       "-x", "ir", str(ir_file), "-x", "c", str(runtime),
                       "-I", str(include), "-o", str(guest)]
            if sanitizers:
                command[1:1] = ["-fsanitize=address,undefined",
                                "-fno-omit-frame-pointer"]
            linked = run(command)
            if linked.returncode != 0:
                raise RuntimeError(name + ": Clang failed: " +
                                   linked.stderr[:1000].decode("utf-8", "replace"))
            before = sha256(guest.read_bytes())
            env = dict(os.environ)
            if sanitizers:
                env["ASAN_OPTIONS"] = "detect_leaks=1:halt_on_error=1"
                env["UBSAN_OPTIONS"] = "halt_on_error=1"
            executed = run([str(guest)], timeout=20, env=env)
            after = sha256(guest.read_bytes())
            if executed.returncode != 42 or executed.stdout or executed.stderr or before != after:
                raise RuntimeError(name + ": guest returned wrong result, diagnostics, or mutated binary")
            print("PASS: " + name + ": source=" + expected_hash +
                  " ir=" + sha256(ir) + " binary=" + before, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--source-root", required=True, type=Path)
    parser.add_argument("--sanitizers", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.compiler.resolve(), args.clang.resolve(),
               args.source_root.resolve(), args.sanitizers)
    except (RuntimeError, OSError, subprocess.SubprocessError) as error:
        print("NATIVE_GUEST_V1_DENIED: " + str(error), file=sys.stderr)
        sys.exit(1)
