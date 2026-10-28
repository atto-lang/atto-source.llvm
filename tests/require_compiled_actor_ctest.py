#!/usr/bin/env python3
"""Require an actual-source actor CTest registration, not just a grep match.

This validates registration only. Its caller MUST run the exact test afterward.
"""
import argparse
import json
from pathlib import Path
import re
import subprocess
import sys

NAME = "wasm32_source_actor_guest_lifecycle"
UNSAFE = {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION"}


class RegistrationFailure(ValueError):
    pass


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
    except (RegistrationFailure, ValueError) as exc:
        print("NOT_EXECUTED: " + str(exc), file=sys.stderr)
        return 1
    print("COMPILED_ACTOR_GUEST_CTEST_REGISTERED_NOT_YET_EXECUTED", NAME)
    return 0


if __name__ == "__main__":
    sys.exit(main())
