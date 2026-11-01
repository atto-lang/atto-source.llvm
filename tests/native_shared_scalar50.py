#!/usr/bin/env python3
"""Run exactly fifty reviewed Oreslang sources via the real LLVM compiler+Clang."""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import tempfile

EXPECTED_BLOB = "a948dbaf594916a6ee07f65d49a0cf5ae398ca7b"


def cases(path: Path):
    if path.is_symlink() or not path.is_file():
        raise AssertionError("scalar50 source corpus must be a regular file")
    data = path.read_bytes()
    if not data.endswith(b"\n") or b"\r" in data or len(data) > 65536:
        raise AssertionError("scalar50 manifest is not canonical LF-only TSV")
    digest = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
    if digest != EXPECTED_BLOB:
        raise AssertionError(f"scalar50 Git blob mismatch: {digest}")
    lines = [line for line in data.decode("utf-8").splitlines()
             if line and not line.startswith("#")]
    if len(lines) != 50:
        raise AssertionError(f"expected 50 sources, found {len(lines)}")
    for index, line in enumerate(lines, 1):
        parts = line.split("\t")
        if len(parts) != 3 or parts[0] != f"scalar-{index:03}":
            raise AssertionError(f"bad scalar50 record {index}")
        key, expected, source = parts
        if not re.fullmatch(r"[1-9][0-9]*", expected) or not (1 <= int(expected) < 128):
            raise AssertionError(f"{key}: malformed expected guest exit status")
        if not re.fullmatch(r"pub fnc main\(\): int \{ return [-+*() 0-9]+; \}", source):
            raise AssertionError(f"{key}: source shape not in reviewed scalar subset")
        yield key, int(expected), source


def run(args):
    for name in ("compiler", "clang"):
        path = getattr(args, name)
        if not path.is_file() or path.is_symlink():
            raise AssertionError(f"missing or symlinked {name} executable")
    with tempfile.TemporaryDirectory(prefix="ores-scalar50-llvm-") as directory:
        work = Path(directory)
        count = 0
        for case_id, expected, source in cases(args.corpus):
            src = work / "case.ores"
            exe = work / "case"
            src.write_text(source + "\n", encoding="utf-8")
            compiled = subprocess.run([str(args.compiler.resolve()), str(src)],
                                      capture_output=True, timeout=30, check=False)
            if (compiled.returncode != 0 or compiled.stderr or
                    not compiled.stdout.startswith(b"; ModuleID") or
                    b"define i32 @main()" not in compiled.stdout):
                raise AssertionError(f"{case_id}: invalid real compiler IR/diagnostic: "
                                     f"rc={compiled.returncode} stderr={compiled.stderr[:600]!r}")
            linked = subprocess.run([str(args.clang.resolve()), "-x", "ir", "-",
                                     "-o", str(exe)], input=compiled.stdout,
                                    capture_output=True, timeout=30, check=False)
            if linked.returncode != 0:
                raise AssertionError(f"{case_id}: generated native IR failed Clang: "
                                     f"{linked.stderr[:600]!r}")
            guest = subprocess.run([str(exe)], capture_output=True,
                                   timeout=10, check=False)
            if (guest.returncode != expected or guest.stdout or guest.stderr):
                raise AssertionError(f"{case_id}: native guest exit={guest.returncode}, "
                                     f"expected={expected}, stderr={guest.stderr[:600]!r}")
            print(f"PASS {case_id} native LLVM exit={expected}", flush=True)
            count += 1
        if count != 50:
            raise AssertionError("incomplete executed native compiler matrix")
    print(f"SHARED_SCALAR50_NATIVE_LLVM_EXECUTED cases={count}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, required=True)
    run(parser.parse_args())
