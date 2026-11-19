#!/usr/bin/env python3
"""Two-layer ores.core / ores.std native driver admission."""
import argparse
import pathlib
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          text=True, timeout=30)


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    a = p.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-library-profile-") as tmp:
        source = pathlib.Path(tmp) / "probe.ores"
        source.write_text("pub fnc probe(int value): int { return value + 1; }\n")

        def compile_with(*args):
            return run([a.compiler, *args, str(source)])

        # Hosted native defaults to ores.std and accepts an explicit std profile.
        host_default = compile_with("--target=host")
        require(host_default.returncode == 0, host_default.stderr)
        host_std = compile_with("--target=host", "--library-profile=std")
        require(host_std.returncode == 0, host_std.stderr)

        # Do not falsely label the ordinary hosted compiler path as freestanding.
        host_core = compile_with("--target=host", "--library-profile=core")
        require(host_core.returncode != 0 and not host_core.stdout,
                "host target incorrectly admitted ores.core execution")
        require("freestanding" in host_core.stderr.lower(), host_core.stderr)

        # Cortex-M is a certified no-OS target and therefore defaults to core.
        cortex_default = compile_with("--target=thumbv7m-none-eabi")
        require(cortex_default.returncode == 0, cortex_default.stderr)
        cortex_core = compile_with("--target=thumbv7m-none-eabi",
                                   "--library-profile=core")
        require(cortex_core.returncode == 0, cortex_core.stderr)
        cortex_std = compile_with("--target=thumbv7m-none-eabi",
                                  "--library-profile=std")
        require(cortex_std.returncode != 0 and not cortex_std.stdout,
                "freestanding target incorrectly admitted ores.std")
        require("ores.std" in cortex_std.stderr, cortex_std.stderr)

        # wasm32-unknown-unknown is likewise admitted only as core today.
        wasm_core = compile_with("--target=wasm32-unknown-unknown",
                                 "--library-profile=core")
        require(wasm_core.returncode == 0, wasm_core.stderr)
        wasm_std = compile_with("--target=wasm32-unknown-unknown",
                                "--library-profile=std")
        require(wasm_std.returncode != 0 and not wasm_std.stdout,
                "wasm32-unknown-unknown incorrectly admitted ores.std")

        for invalid in ["alloc", "actor", "otp", "hosted",
                        "freestanding-core", "CORE", ""]:
            bad = compile_with("--library-profile=" + invalid)
            require(bad.returncode != 0 and not bad.stdout,
                    "old/invalid library profile admitted: " + invalid)
            require("core or std" in bad.stderr, bad.stderr)

        duplicate = compile_with("--library-profile=std", "--library-profile=std")
        require(duplicate.returncode != 0 and not duplicate.stdout,
                "duplicate library profile admitted")
        require("duplicate library-profile" in duplicate.stderr,
                duplicate.stderr)

    print("ores.core / ores.std native profile admission PASS")


if __name__ == "__main__":
    main()
