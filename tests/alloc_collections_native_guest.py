#!/usr/bin/env python3
"""Execute actual LLVM Atto guest collections; host/source checks do not count.

This intentionally fails when the native frontend cannot lower the admitted
module. It never substitutes a Python/C++ collection model for guest output.
"""
import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parents[1]
GUESTS = ROOT / "tests/fixtures/alloc-collections-v1"


def run(*args):
    return subprocess.run(
        [str(a) for a in args],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=120,
        check=False,
    )


def require(condition, diagnostic):
    if not condition:
        raise AssertionError(diagnostic)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    compiler = pathlib.Path(args.compiler).resolve(strict=True)
    clang = pathlib.Path(args.clang).resolve(strict=True)
    runtime = ROOT / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    include = ROOT / "src/main/cpp"
    require(runtime.is_file(), f"missing explicit native Array provider: {runtime}")

    with tempfile.TemporaryDirectory(prefix="atto-llvm-alloc-real-guest-") as temp:
        tmp = pathlib.Path(temp)
        for module in ("bitset", "linked_list", "map"):
            fixture = GUESTS / f"{module}.atto"
            require(fixture.is_file(), f"missing .atto guest {fixture}")
            emitted = run(compiler, fixture)
            require(emitted.returncode == 0,
                    f"{module}: native compiler failed: {emitted.stderr}")
            require("define " in emitted.stdout and "main" in emitted.stdout,
                    f"{module}: successful compiler invocation emitted no guest IR")
            require("unknown compiler-owned" not in emitted.stderr.lower(),
                    f"{module}: compiler silently ignored alloc import")
            ir = tmp / (module + ".ll")
            binary = tmp / module
            ir.write_text(emitted.stdout, encoding="utf-8")
            linked = run(
                clang, "-O1", "-Wno-override-module", "-std=c11",
                "-x", "ir", ir, "-x", "c", runtime, "-I", include,
                "-o", binary,
            )
            require(linked.returncode == 0,
                    f"{module}: failed to link compiler-emitted guest IR: {linked.stderr}")
            executed = run(binary)
            require(executed.returncode == 42,
                    f"{module}: real guest exited {executed.returncode}, expected 42; stderr={executed.stderr}")
            print(f"ATTO_LLVM_ALLOC_GUEST|{module}|executed=1|exit=42")

        for invalid in ("unknown", "traversal"):
            fixture = GUESTS / f"{invalid}.atto"
            denied = run(compiler, fixture)
            require(denied.returncode != 0,
                    f"{invalid}: forbidden module unexpectedly accepted")
            require(not denied.stdout.strip(),
                    f"{invalid}: failed compiler leaked IR or stdout")
            require("unknown compiler-owned alloc module" in denied.stderr,
                    f"{invalid}: unexpected failure, not the alloc import gate: {denied.stderr}")

        # Never admit implicit host malloc or a hidden provider in bare metal.
        for target in ("wasm32-unknown-unknown", "thumbv7m-none-eabi"):
            attempt = run(
                compiler, "--library-profile=core", "--target=" + target,
                GUESTS / "map.atto",
            )
            require(attempt.returncode != 0 and not attempt.stdout.strip(),
                    f"{target}: providerless freestanding alloc accepted or emitted IR")
            require("freestanding alloc/* requires an explicit allocator provider"
                    in attempt.stderr,
                    f"{target}: denied for wrong reason: {attempt.stderr}")
        print("ATTO_LLVM_ALLOC_ADMISSION|negative=4|executed=1")


if __name__ == "__main__":
    main()
