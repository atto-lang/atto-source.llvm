#!/usr/bin/env python3
"""Executable, byte-pinned Oreslang borrowed-class v2 subset.

Guest Result<T,E>, imported class-owned arrays and suspended loans remain
UNIMPLEMENTED. This test cannot certify those promises.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

FIXTURES = {
    "borrow-mut-scalar": (
        """define class Cell as
  pub let int value = 0;
end
fnc set_value(borrow mut Cell cell): void {
  cell.value = 42;
  return;
}
pub fnc main(): int {
  let mut Cell cell = new Cell();
  set_value(rt borrow mut cell);
  return cell.value;
}
""",
        "360e5cd946d1a9a55abe5db711b723b96ddb6e64fc303efe7a7ed71505ba0c2d",
        "return", None,
    ),
    "borrow-overlap-reject": (
        """define class Cell as
  pub let int value = 0;
end
fnc inspect(borrow mut Cell writer, borrow Cell reader): int {
  return writer.value;
}
pub fnc main(): int {
  let mut Cell cell = new Cell();
  return inspect(rt borrow mut cell, rt borrow cell);
}
""",
        "ddc2d0088d95390a7df4ec35a7feb3c81003c55fe335714780ef99ac7aa6e7e1",
        "reject", b"conflicting active native borrow",
    ),
    "borrow-shared-write-reject": (
        """define class Cell as
  pub let int value = 0;
end
fnc mutate(borrow Cell cell): void {
  cell.value = 42;
  return;
}
pub fnc main(): int {
  let Cell cell = new Cell();
  mutate(rt borrow cell);
  return 42;
}
""",
        "9e8a882482c607af20922c93eb00b1e4624b6b3da4ae6352770edd76b89f1109",
        "reject", b"ownership violation",
    ),
}


def run(args, timeout=90):
    result = subprocess.run(args, input=b"", capture_output=True, timeout=timeout, check=False)
    if len(result.stdout) > 131072 or len(result.stderr) > 131072:
        raise RuntimeError("compiler or native guest exceeded output budget")
    return result


def check(compiler, clang):
    for exe in (compiler, clang):
        if not exe.is_file():
            raise RuntimeError("native compiler or Clang missing")
    with tempfile.TemporaryDirectory(prefix="ores-borrow-v2-") as td:
        work = Path(td)
        for name, (source, pinned, kind, diagnostic) in FIXTURES.items():
            blob = source.encode("utf-8")
            if hashlib.sha256(blob).hexdigest() != pinned:
                raise RuntimeError("canonical borrow fixture SHA-256 mismatch: " + name)
            file = work / (name + ".ores")
            file.write_bytes(blob)
            compiled = run([str(compiler), str(file)])
            if kind == "reject":
                if (compiled.returncode != 1 or compiled.stdout or
                        not compiled.stderr or diagnostic not in compiled.stderr):
                    raise RuntimeError(name + ": not rejected before IR with expected diagnostic")
                print("PASS: " + name + ": no LLVM IR; expected diagnostic", flush=True)
                continue
            if compiled.returncode != 0 or compiled.stderr or b"define " not in compiled.stdout:
                raise RuntimeError(name + ": guest compiler failed")
            ir_file = work / "borrow-v2.ll"
            ir_file.write_bytes(compiled.stdout)
            binary = work / "borrow-v2"
            linked = run([str(clang), "-O1", "-x", "ir", str(ir_file), "-o", str(binary)])
            if linked.returncode or linked.stderr:
                raise RuntimeError(name + ": cannot link real LLVM guest")
            before = hashlib.sha256(binary.read_bytes()).hexdigest()
            executed = run([str(binary)], timeout=20)
            after = hashlib.sha256(binary.read_bytes()).hexdigest()
            if executed.returncode != 42 or executed.stdout or executed.stderr or before != after:
                raise RuntimeError(name + ": native guest result/binary mismatch")
            print("PASS: " + name + ": IR=" +
                  hashlib.sha256(compiled.stdout).hexdigest() +
                  " native-binary=" + before, flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    try:
        check(args.compiler.resolve(), args.clang.resolve())
    except (RuntimeError, OSError, subprocess.TimeoutExpired) as error:
        print("BORROW_V2_DENIED: " + str(error), file=sys.stderr)
        sys.exit(1)
