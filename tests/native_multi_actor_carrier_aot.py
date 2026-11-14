#!/usr/bin/env python3
"""Compile two distinct Oreslang sources, then use one real native carrier."""
import argparse
import pathlib
import subprocess
import tempfile

def main():
    p = argparse.ArgumentParser()
    for name in ("compiler", "clangxx", "fast", "slow", "test", "source_root"):
        p.add_argument("--" + name.replace("_","-"), required=True)
    a = p.parse_args()
    def run(argv, **kwargs):
        return subprocess.run(argv, capture_output=True, text=True, check=True,
                              timeout=90, **kwargs)
    with tempfile.TemporaryDirectory(prefix="oreslang-multi-actor-") as tmp:
        root = pathlib.Path(tmp)
        irs = []
        for name, path in (("slow", a.slow), ("fast", a.fast)):
            ir = run([a.compiler, "--experimental-rt-scalar-loop", path]).stdout
            assert "define i32 @oreslang_rt_scalar_frame_step" in ir
            # Two independent source modules have the same experimental ABI
            # symbol names; give each test module a distinct C symbol prefix.
            ir = ir.replace("oreslang_rt_scalar_frame_", "oreslang_rt_" + name + "_frame_")
            dst = root / (name + ".ll")
            dst.write_text(ir)
            irs.append(dst)
        s = pathlib.Path(a.source_root)
        for label, flags in [
            ("release", ["-O3", "-DNDEBUG"]),
            ("checked", ["-O1", "-g", "-fsanitize=address,undefined",
                         "-fno-omit-frame-pointer"])]:
            binary = root / label
            compile_command = [
                a.clangxx, "-std=c++17", "-pthread", "-Wall", "-Wextra",
                "-Wpedantic", "-Werror", "-Wno-error=override-module", *flags,
                "-I", str(s), a.test,
                str(s / "oreslang/runtime/MultiActorCarrier.cpp"),
                "-x", "ir", *map(str, irs), "-o", str(binary)]
            run(compile_command)
            # Repeated process-isolated schedules exercise cancellation and
            # producer admission ordering across different OS interleavings.
            for attempt in range(12 if label == "release" else 3):
                result = run([str(binary)])
                assert "TWO_COMPILED_ACTORS_ONE_REAL_CARRIER_PASS" in result.stdout, (
                    label, attempt, result.stdout, result.stderr)
        print("TWO_COMPILED_ACTORS_ONE_REAL_CARRIER_PASS: 12 Release + 3 ASan/UBSan schedules")

if __name__ == "__main__":
    main()
