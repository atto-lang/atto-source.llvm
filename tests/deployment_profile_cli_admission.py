#!/usr/bin/env python3
"""Fail-closed compiler admission: profiles must never emit generic host IR."""
import argparse
import subprocess

PROFILES = ("ebpf-xdp", "ebpf-tc", "carrier-linux", "fpga-hls", "asic-rtl")

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    for profile in PROFILES:
        for prefix in ((), ("--target=host",), ("--target=wasm32",)):
            completed = subprocess.run(
                (args.compiler, *prefix, "--deployment-profile=" + profile,
                 "/nonexistent/oreslang-source.ores"),
                capture_output=True, text=True, check=False, timeout=15)
            assert completed.returncode == 2, (profile, prefix, completed)
            assert not completed.stdout, (profile, prefix, completed.stdout)
            assert ("deployment profile '" + profile + "'") in completed.stderr
            assert "refusing host fallback" in completed.stderr
    negatives = (
        ("--deployment-profile=ebpf-XDP",),
        ("--deployment-profile=",),
        ("--deployment-profile=ebpf-xdp,host",),
        ("--deployment-profile",),
        ("--deployment-profile=ebpf-xdp", "--deployment-profile=asic-rtl"),
    )
    for command in negatives:
        completed = subprocess.run(
            (args.compiler, *command, "/nonexistent/oreslang-source.ores"),
            capture_output=True, text=True, check=False, timeout=15)
        assert completed.returncode == 2, (command, completed)
        assert not completed.stdout, (command, completed.stdout)
    print("PASS: 15 target/profile combinations and 5 invalid/duplicate forms fail closed")

if __name__ == "__main__":
    main()
