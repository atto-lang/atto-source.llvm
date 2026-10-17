#!/usr/bin/env python3
"""Prove native std/otp imports remain explicit fail-closed until implemented."""

import argparse
import pathlib
import subprocess
import sys
import tempfile

MODULES = (
    "gen_server",
    "supervisor",
    "gen_statem",
    "application",
    "event_manager",
    "registry",
)
DIAGNOSTIC = "native module imports unsupported until typed module/linker lowering"


def require(condition, detail):
    if not condition:
        raise RuntimeError(detail)


def run(cmd):
    return subprocess.run(
        cmd, capture_output=True, text=True, check=False, timeout=20
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--positive-fixture", required=True)
    args = parser.parse_args()

    control = run([args.compiler, args.positive_fixture])
    require(control.returncode == 0,
            "positive control did not compile")
    require(control.stdout.startswith("; ModuleID"),
            "positive control did not emit LLVM IR")
    require(not control.stderr,
            "positive control emitted stderr")

    with tempfile.TemporaryDirectory(prefix="ores-otp-native-reject-") as root:
        root = pathlib.Path(root)
        for module in MODULES:
            fixture = root / (module + ".ores")
            alias = "".join(part.capitalize() for part in module.split("_"))
            fixture.write_text(
                'import module ' + alias + ' from "std/otp/' + module + '";\n'
                'pub fnc main(): int { return 0; }\n',
                encoding="utf-8",
            )
            result = run([args.compiler, str(fixture)])
            require(result.returncode == 1,
                    module + ": unsupported import must exit exactly 1")
            require(not result.stdout,
                    module + ": compiler emitted LLVM IR after rejected import")
            require(DIAGNOSTIC in result.stderr,
                    module + ": explicit import diagnostic missing: " + result.stderr)

    print("OTP_STDLIB_NATIVE_FAIL_CLOSED_6_PASS")


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("otp import fail-closed test failed: " + str(error), file=sys.stderr)
        sys.exit(1)
