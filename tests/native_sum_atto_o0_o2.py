#!/usr/bin/env python3
"""Real LLVM .atto Option/Result guest parity at Clang O0 and O2.

Requires a source-built oreslang-llvmc and actual Clang native processes.
This is not synthetic IR, a parser-only canary, or a host-only resolver test.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

POSITIVES = (
    ("native-sum-option-bool-atto.atto", 42),
    ("native-sum-result-string-atto.atto", 42),
    ("native-sum-option-none-atto.atto", 42),
    ("native-sum-result-ok-atto.atto", 42),
)
NEGATIVES = (
    ("native-sum-option-bool-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-result-string-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-option-bool-conditional-invalid-atto.atto",
     "native sum conditional branch payload types do not match"),
    ("native-sum-result-string-conditional-invalid-atto.atto",
     "native sum conditional branch payload types do not match"),
    ("native-sum-option-bool-fallback-invalid-atto.atto",
     "native sum unwrap_or fallback type does not match payload"),
    ("native-sum-result-fallback-invalid-atto.atto",
     "native sum unwrap_or fallback type does not match payload"),
    ("native-sum-result-ok-invalid-atto.atto",
     "native sum constructor payload type does not match declared variant"),
    ("native-sum-option-none-arity-invalid-atto.atto",
     "native sum constructor requires its exact positional payload arity"),
)

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def run(command, *, stdin=None, timeout=60):
    return subprocess.run(command, input=stdin, capture_output=True,
                          check=False, timeout=timeout)

def sha(data):
    return hashlib.sha256(data).hexdigest()

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    compiler = Path(args.compiler).resolve(strict=True)
    clang = Path(args.clang).resolve(strict=True)
    require(compiler.is_file() and compiler.name == "oreslang-llvmc",
            "native guest parity requires real oreslang-llvmc")
    require(clang.is_file(), "native guest parity requires real clang")
    fixtures = root / "tests" / "fixtures"
    regions = (root / "src/main/cpp/oreslang/runtime/NativeRecordRegion.c",
               root / "src/main/cpp/oreslang/runtime/NativeStringRegion.c")
    for region in regions:
        require(region.is_file() and not region.is_symlink(),
                "native region source missing or symlinked")
    count = 0
    with tempfile.TemporaryDirectory(prefix="atto-native-sum-") as temp:
        temporary = Path(temp)
        for filename, expected in POSITIVES:
            source = fixtures / filename
            require(source.is_file() and not source.is_symlink() and
                    source.suffix == ".atto", "invalid positive .atto fixture")
            original = source.read_bytes()
            ir_hashes = []
            receipts = []
            for level in (0, 2):
                generated = run([str(compiler), "--target=host", str(source)])
                require(generated.returncode == 0 and
                        generated.stdout.startswith(b"; ModuleID") and
                        not generated.stderr,
                        "real frontend rejected positive " + filename)
                ir_hashes.append(sha(generated.stdout))
                executable = temporary / (filename + ".O" + str(level))
                linked = run([str(clang), "-O" + str(level), "-x", "ir", "-",
                              "-x", "c", "-std=c11", "-pthread",
                              "-I", str(root / "src/main/cpp"),
                              *(str(region) for region in regions),
                              "-o", str(executable)], stdin=generated.stdout)
                require(linked.returncode == 0 and executable.is_file(),
                        "real Clang link failed for " + filename +
                        " O" + str(level))
                header = executable.read_bytes()[:4]
                if sys.platform == "darwin":
                    require(header in (b"\xfe\xed\xfa\xce", b"\xce\xfa\xed\xfe",
                                       b"\xfe\xed\xfa\xcf", b"\xcf\xfa\xed\xfe"),
                            "native binary is not Mach-O on macOS")
                else:
                    require(header == b"\x7fELF",
                            "native binary is not ELF on Linux")
                native = run([str(executable)], timeout=10)
                require(native.returncode == expected,
                        "native guest returned wrong exit code: " + filename +
                        " O" + str(level))
                require(source.read_bytes() == original, "fixture mutated")
                receipts.append("NATIVE_ATTO_SUM_PASS|source=" + filename +
                                "|opt=O" + str(level) +
                                "|exit=" + str(expected) +
                                "|source_sha256=" + sha(original) +
                                "|ir_sha256=" + ir_hashes[-1] +
                                "|binary_sha256=" +
                                sha(executable.read_bytes()))
            require(ir_hashes[0] == ir_hashes[1],
                    "same source generated different frontend IR for O0/O2")
            for receipt in receipts:
                print(receipt)
                count += 1
        negatives = 0
        for filename, diagnostic in NEGATIVES:
            source = fixtures / filename
            require(source.is_file() and not source.is_symlink() and
                    source.suffix == ".atto", "invalid negative fixture")
            original = source.read_bytes()
            # Admission runs in oreslang-llvmc, before Clang optimization.
            # Retesting the same rejection twice is not an O0/O2 test.
            rejected = run([str(compiler), "--target=host", str(source)])
            require(rejected.returncode == 1, "bad source not rejected")
            require(not rejected.stdout, "failed compiler emitted LLVM IR")
            require(diagnostic.encode() in rejected.stderr,
                    "wrong rejection diagnostic for " + filename)
            require(source.read_bytes() == original, "negative source mutated")
            negatives += 1
    require(count == 2 * len(POSITIVES) and negatives == len(NEGATIVES),
            "native parity matrix incomplete")
    print("NATIVE_ATTO_SUM_MATRIX_PASS|executed=" + str(count) +
          "|compiler_rejections=" + str(negatives) +
          "|clang_levels=O0,O2|synthetic_ir=NO")

if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("native AttoLang typed-sum test failed: " + str(error),
              file=sys.stderr)
        sys.exit(1)
