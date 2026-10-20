#!/usr/bin/env python3
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
args = parser.parse_args()

with tempfile.TemporaryDirectory(prefix="ores-profile-") as directory:
    source = pathlib.Path(directory) / "main.ores"
    source.write_text("fnc answer(): int { return 42; }\n")

    for flag in ("--release", "--strict-aot", "--hard-rt"):
        result = subprocess.run(
            [args.compiler, flag, str(source)],
            text=True, capture_output=True)
        assert result.returncode == 2, (flag, result.returncode, result.stderr)
        assert "rejected rather than weakened" in result.stderr, result.stderr

    conflict = subprocess.run(
        [args.compiler, "--release", "--strict-aot", str(source)],
        text=True, capture_output=True)
    assert conflict.returncode == 2
    assert "mutually exclusive" in conflict.stderr

    hosted = subprocess.run(
        [args.compiler, "--freestanding", str(source)],
        text=True, capture_output=True)
    assert hosted.returncode == 2
    assert "thumbv7m-none-eabi" in hosted.stderr

    cortex = subprocess.run(
        [args.compiler, "--freestanding", "--target=thumbv7m-none-eabi", str(source)],
        text=True, capture_output=True)
    assert cortex.returncode == 0, cortex.stderr
    assert "target triple" in cortex.stdout or "define" in cortex.stdout

print("build profile CLI admission: PASS")
