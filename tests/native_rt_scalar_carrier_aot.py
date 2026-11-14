#!/usr/bin/env python3
"""Compile exact Oreslang scalar-loop guest IR against the real HungryCarrier."""
import argparse
import pathlib
import subprocess
import tempfile

def main():
    p = argparse.ArgumentParser()
    for name in ("compiler", "clang", "clangxx", "fixture", "test", "source_root"):
        p.add_argument("--" + name.replace("_", "-"), required=True)
    args = p.parse_args()
    def run(argv, **kwargs):
        return subprocess.run(argv, check=True, capture_output=True, text=True,
                              timeout=120, **kwargs)
    with tempfile.TemporaryDirectory(prefix="oreslang-carrier-cooperate-") as d:
        folder = pathlib.Path(d)
        ir = folder / "compiled.llvm.ll"
        compiled = run([args.compiler, "--experimental-rt-scalar-loop", args.fixture])
        assert "define i32 @oreslang_rt_scalar_frame_step" in compiled.stdout
        ir.write_text(compiled.stdout)
        src = pathlib.Path(args.source_root)
        for opt in (["-O2"], ["-O3", "-DNDEBUG"]):
            executable = folder / ("carrier-opt-" + str(len(opt)))
            flags = ["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror",
                     "-Wno-error=override-module", "-pthread"]
            cmd = [args.clangxx, *flags, *opt, "-I", str(src),
                   args.test, str(src / "oreslang/runtime/HungryCarrier.cpp"),
                   str(src / "oreslang/runtime/ParkingSignal.cpp"),
                   "-x", "ir", str(ir), "-o", str(executable)]
            run(cmd)
            result = run([str(executable)])
            assert "NATIVE_COMPILED_SCALAR_CARRIER_COOPERATE_PASS" in result.stdout
        print("NATIVE_COMPILED_SCALAR_CARRIER_COOPERATE_PASS")

if __name__ == "__main__":
    main()
