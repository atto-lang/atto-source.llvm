#!/usr/bin/env python3
"""Restricted common v0 advisory facts from an executable compiler's real syntax gate.

Only one literal-returning main is accepted. This narrow grammar deliberately
does NOT infer general language effects from compiler exit code or IR strings.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile

BACKEND = "llvm"
# Deliberately no comments, imported declarations, dynamic calls, loops or actors.
# Do NOT broaden without consuming typed, compiler-issued effect metadata.
SCALAR = re.compile(
    rb"\A[ \t\r\n]*(?:pub[ \t\r\n]+)?fnc[ \t\r\n]+main"
    rb"[ \t\r\n]*\([ \t\r\n]*\)[ \t\r\n]*:[ \t\r\n]*int"
    rb"[ \t\r\n]*\{[ \t\r\n]*return[ \t\r\n]+"
    rb"(?:0|[1-9][0-9]{0,17})[ \t\r\n]*;[ \t\r\n]*\}[ \t\r\n]*\Z",
    re.ASCII,
)
MAX_SOURCE = 1_048_576
MAX_IR = 2 * 1_048_576


def admitted_bytes(source: bytes) -> None:
    if len(source) > MAX_SOURCE or not SCALAR.fullmatch(source):
        raise ValueError("unsupported source: only one no-effect literal main is admitted")
    source.decode("utf-8", errors="strict")


def facts(source: bytes) -> dict:
    admitted_bytes(source)
    return {
        "version": "ores-static-facts/v0",
        "unit": "sha256:" + hashlib.sha256(source).hexdigest(),
        "procedures": [{"id": "__root__.main/0", "loops": []}],
        "edges": [], "taint_sources": [], "flows": [], "sinks": [],
        "permission_checks": [],
    }


def exact_source(path: Path) -> bytes:
    before = path.lstat()
    if not stat.S_ISREG(before.st_mode) or before.st_size > MAX_SOURCE:
        raise ValueError("source must be a bounded regular non-symlink file")
    flags = os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        first = os.fstat(fd)
        if (first.st_dev, first.st_ino) != (before.st_dev, before.st_ino):
            raise ValueError("source changed before snapshot")
        with os.fdopen(fd, "rb", closefd=False) as stream:
            data = stream.read(MAX_SOURCE + 1)
        last = os.fstat(fd)
        for field in ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns"):
            if getattr(first, field) != getattr(last, field):
                raise ValueError("source changed during snapshot")
        return data
    finally:
        os.close(fd)


def certified_parser_admission(compiler: Path, source: bytes) -> None:
    executable = compiler.resolve(strict=True)
    if not executable.is_file():
        raise ValueError("compiler must be a local executable regular file")
    with tempfile.TemporaryDirectory(prefix="ores-facts-v0-") as tmp:
        staged = Path(tmp) / "admitted.ores"
        output = Path(tmp) / "backend.ir"
        staged.write_bytes(source)
        staged.chmod(0o400)
        cmd = ([str(executable), "ir", str(staged)]
               if BACKEND == "forge" else [str(executable), str(staged)])
        with output.open("wb") as out:
            run = subprocess.run(cmd, stdout=out, stderr=subprocess.DEVNULL,
                                 timeout=30, check=False)
        if run.returncode != 0:
            raise ValueError("real backend compiler rejected the scalar source")
        if staged.read_bytes() != source:
            raise ValueError("staged source was modified by the compiler")
        if output.stat().st_size <= 0 or output.stat().st_size > MAX_IR:
            raise ValueError("backend IR is empty or exceeds its output budget")
        produced = output.read_bytes()
        expected = b"ret %" if BACKEND == "forge" else b"ret i64"
        if expected not in produced:
            raise ValueError("compiler returned no recognizable checked scalar IR")


def self_test() -> None:
    good = b"pub fnc main(): int { return 42; }\n"
    f = facts(good)
    assert f["version"] == "ores-static-facts/v0"
    assert f["unit"] == "sha256:" + hashlib.sha256(good).hexdigest()
    assert len(f["procedures"]) == 1 and f["edges"] == []
    for bad in (
        b"", b"fnc main():int{return foo();}",
        b"pub fnc main():int{while true {} return 42;}",
        b"pub fnc main():int{return 42;} fnc backdoor():int{return 1;}",
        b"\xef\xbb\xbf" + good, b"fnc main():int{return 42;}\x00",
        b"fnc main():int{return 42;// fake\n}",
    ):
        try:
            facts(bad)
        except ValueError:
            continue
        raise AssertionError("unsupported source was admitted: " + repr(bad))
    print("static-facts v0 narrow source admission self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.compiler is None or args.source is None:
        parser.error("--compiler and --source are both required")
    source = exact_source(args.source)
    record = facts(source)  # rejects unsupported source before compiler execution
    certified_parser_admission(args.compiler, source)
    print(json.dumps(record, sort_keys=True, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.TimeoutExpired) as exc:
        print("ORES-ANALYZE-UNSUPPORTED: " + str(exc), file=sys.stderr)
        raise SystemExit(2)
