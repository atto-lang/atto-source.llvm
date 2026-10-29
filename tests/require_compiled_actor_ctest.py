#!/usr/bin/env python3
"""Require an actual-source actor CTest registration, not just a grep match.

This validates registration only. Its caller MUST run the exact test afterward.
"""
import argparse
import hashlib
import json
import os
import stat
from pathlib import Path
import re
import subprocess
import sys

NAME = "wasm32_source_actor_guest_lifecycle"
UNSAFE = {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION"}

# Exact reviewed guest harness and fixtures; registration alone is insufficient.
# Updating any expected blob requires explicit review of this gate and its tests.
PINNED_GUEST_BLOBS = {
    "tests/wasm32_source_actor_guest.py": "7884a57900bb2f06bd2046436d157ffb922bfaef",
    "tests/fixtures/wasm32-single-source-actor.ores": "adfc0b43463d7ce92d32a4b1b63602f91e527329",
    "tests/fixtures/source-actor-spawn-stop.ores": "62b5a79990324929690b8acb7021e35bf7a09f65",
    "tests/fixtures/source-actor-send.ores": "3050260bd65b019a2b00191fba9086971409511b",
}


class RegistrationFailure(ValueError):
    pass


def verify_guest_inputs(source_dir: Path, pins: dict[str, str] = PINNED_GUEST_BLOBS) -> None:
    """Require the exact real guest harness and fixtures, not replaced no-op files.

    `pins` is injectable for hermetic regression tests; the CLI always uses
    immutable PINNED_GUEST_BLOBS. This does NOT prove runner execution.
    """
    if source_dir.is_symlink() or source_dir.resolve() != source_dir.absolute():
        raise RegistrationFailure("source directory must not be a symlink")
    if any((source_dir / d).is_symlink() for d in ("tests", "tests/fixtures")):
        raise RegistrationFailure("guest input parent directory must not be a symlink")
    if not pins or any(not isinstance(rel, str) or not isinstance(pin, str)
                       or not re.fullmatch(r"[0-9a-f]{40}", pin)
                       for rel, pin in pins.items()):
        raise RegistrationFailure("invalid reviewed guest blob pins")
    for relative, expected_blob in pins.items():
        if relative.startswith("/") or ".." in Path(relative).parts:
            raise RegistrationFailure("unsafe guest input path")
        file = source_dir / relative
        if file.is_symlink():
            raise RegistrationFailure("guest harness or fixture is a symlink: " + relative)
        try:
            fd = os.open(file, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            with os.fdopen(fd, "rb") as stream:
                info = os.fstat(stream.fileno())
                if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= 65536:
                    raise RegistrationFailure("guest input not a bounded regular file: " + relative)
                contents = stream.read(65537)
                if len(contents) != info.st_size:
                    raise RegistrationFailure("guest input size changed: " + relative)
        except OSError as exc:
            raise RegistrationFailure("guest input unavailable: " + relative) from exc
        blob = hashlib.sha1(b"blob " + str(len(contents)).encode("ascii") +
                            b"\x00" + contents).hexdigest()
        if blob != expected_blob:
            raise RegistrationFailure("guest harness or fixture blob mismatch: " + relative)


def verify_registration(manifest: object, *, source_dir: Path, build_dir: Path) -> None:
    if not isinstance(manifest, dict) or manifest.get("kind") != "ctestInfo":
        raise RegistrationFailure("wrong CTest JSON kind")
    version = manifest.get("version")
    if not isinstance(version, dict) or version.get("major") != 1:
        raise RegistrationFailure("unsupported CTest JSON version")
    tests = manifest.get("tests")
    if not isinstance(tests, list):
        raise RegistrationFailure("missing CTest tests array")
    registered = [x for x in tests if isinstance(x, dict) and x.get("name") == NAME]
    if len(registered) != 1:
        raise RegistrationFailure("compiled source actor CTest missing or duplicated")
    test = registered[0]
    command = test.get("command")
    if not isinstance(command, list) or len(command) != 10 or not all(isinstance(v, str) for v in command):
        raise RegistrationFailure("compiled actor CTest command has wrong argument shape")
    if not re.fullmatch(r"python(?:3(?:\.\d+)?)?", Path(command[0]).name):
        raise RegistrationFailure("compiled actor CTest must use Python interpreter")
    if command[1] != str(source_dir / "tests" / "wasm32_source_actor_guest.py"):
        raise RegistrationFailure("compiled actor CTest invokes wrong source harness")
    if command[2::2] != ["--compiler", "--clang", "--node", "--fixtures"]:
        raise RegistrationFailure("compiled actor CTest has wrong flags")
    if command[3] != str(build_dir / "oreslang-llvmc") or \
            command[9] != str(source_dir / "tests" / "fixtures"):
        raise RegistrationFailure("compiled actor CTest compiler or fixture substitution")
    if not all(Path(command[i]).is_absolute() for i in (0, 1, 3, 5, 7, 9)):
        raise RegistrationFailure("compiled actor CTest executable/fixture paths must be absolute")
    properties = test.get("properties", [])
    if not isinstance(properties, list) or any(not isinstance(p, dict) for p in properties):
        raise RegistrationFailure("malformed CTest properties")
    if any(p.get("name") in UNSAFE for p in properties):
        raise RegistrationFailure("compiled actor CTest is skipped, disabled or inverted")
    timeouts = [p.get("value") for p in properties if p.get("name") == "TIMEOUT"]
    if len(timeouts) != 1 or str(timeouts[0]) not in {"120", "120.0"}:
        raise RegistrationFailure("compiled actor CTest must retain 120 second timeout")


def main() -> int:
    a = argparse.ArgumentParser()
    a.add_argument("--source-dir", type=Path, required=True)
    a.add_argument("--build-dir", type=Path, required=True)
    args = a.parse_args()
    if not args.source_dir.is_absolute() or not args.build_dir.is_absolute():
        print("NOT_EXECUTED: required absolute source/build paths", file=sys.stderr)
        return 1
    p = subprocess.run(["ctest", "--test-dir", str(args.build_dir), "--show-only=json-v1"],
                       capture_output=True, text=True, timeout=20)
    if p.returncode != 0:
        print("NOT_EXECUTED: CTest inventory unavailable", file=sys.stderr)
        return 1
    try:
        verify_registration(json.loads(p.stdout), source_dir=args.source_dir, build_dir=args.build_dir)
        verify_guest_inputs(args.source_dir)
    except (RegistrationFailure, ValueError) as exc:
        print("NOT_EXECUTED: " + str(exc), file=sys.stderr)
        return 1
    print("COMPILED_ACTOR_GUEST_CTEST_REGISTERED_NOT_YET_EXECUTED", NAME)
    return 0


if __name__ == "__main__":
    sys.exit(main())
