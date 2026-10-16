#!/usr/bin/env python3
"""Run the common signed-i64 corpus through the *real* native LLVM compiler.

Unlike the reference-only oracle, this compiles Oreslang source to LLVM IR,
renames only LLVM's checked C `main` wrapper and directly invokes the exported
`__oreslang_guest_main(): i64` from a C launcher. This compares the full 64-bit
signed result rather than the truncated shell exit status. No stub passes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "contracts/v1/numeric-parity/source-i64-cases.json"
ENTRY = re.compile(r"(?m)^define\s+i64\s+@__oreslang_guest_main\s*\(\s*\)")
HOST_WRAPPER = re.compile(r"(?m)^(define\s+i32\s+)@main(?=\s*\()")


def run(argv, **kwargs):
    result = subprocess.run(argv, text=True, capture_output=True, timeout=30, check=False, **kwargs)
    if result.returncode != 0 or result.stderr and kwargs.get("input") is not None:
        # The IR linker is allowed normal diagnostic warnings; callers
        # explicitly verify the exit code and stdout they need.
        if result.returncode:
            raise AssertionError(f"native command failed ({argv[0]}): {result.stderr[:1000]}")
    return result


def check_positive(compiler, clang, source, expected, folder, label):
    file = folder / "case.ores"
    file.write_text("pub fnc main():int{return " + source + ";}\n", encoding="utf-8")
    emitted = run([compiler, "--target=host", str(file)])
    ir = emitted.stdout
    if not ENTRY.search(ir):
        raise AssertionError(label + ": real compiler did not emit define i64 @__oreslang_guest_main()")
    # Rename exactly the i32 OS wrapper, not arbitrary mentions of @main
    # in comments, other signatures, globals, or potential diagnostics.
    renamed, wrapper_count = HOST_WRAPPER.subn(
        r"\1@oreslang_os_main", ir)
    if wrapper_count != 1:
        raise AssertionError(label + ": expected exactly one checked OS wrapper")
    object_file = folder / "case.o"
    object_result = run([clang, "-x", "ir", "-c", "-o", str(object_file), "-"], input=renamed)
    if object_result.returncode:
        raise AssertionError(label + ": LLVM IR compile failed")
    host = folder / "host.c"
    host.write_text(
        '#include <stdint.h>\n#include <stdio.h>\n'
        'extern int64_t __oreslang_guest_main(void);\n'
        'int main(void) { printf("%lld\\n", (long long)__oreslang_guest_main()); return 0; }\n',
        encoding="utf-8")
    exe = folder / "numeric-guest"
    run([clang, str(host), str(object_file), "-o", str(exe)])
    observed = run([str(exe)])
    if observed.stdout != str(expected) + "\n":
        raise AssertionError(label + ": expected " + str(expected)
                             + " observed " + repr(observed.stdout))
    if observed.stderr:
        raise AssertionError(label + ": guest wrote unexpected stderr")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve(strict=True))
    clang = str(Path(args.clang).resolve(strict=True))
    raw = CASES.read_bytes()
    actual = hashlib.sha1(
        b"blob " + str(len(raw)).encode("ascii") + bytes((0,)) + raw).hexdigest()
    if actual != "bbc319c19ecd007109d33592e210091eb267e5a2":
        raise AssertionError("native guest corpus differs from reviewed Git blob")
    corpus = json.loads(raw)
    assert corpus["schema_version"] == "1.0.0"
    assert len(corpus["cases"]) == 7
    # The same four negative source expressions must be checked in every
    # backend. Pin the exact reviewed Git blob before using its source bytes.
    overflow_raw = (ROOT / "contracts/v1/numeric-parity/source-i64-overflow-cases.json").read_bytes()
    overflow_sha = hashlib.sha1(
        b"blob " + str(len(overflow_raw)).encode("ascii") + bytes((0,)) +
        overflow_raw).hexdigest()
    if overflow_sha != "2ce0b0ff5c52e54a788280fd64d67fa8dc32614a":
        raise AssertionError("shared source-i64 overflow corpus Git blob mismatch")
    overflow = json.loads(overflow_raw)
    if (overflow.get("schema_version") != "1.0.0"
            or overflow.get("policy") != "no-wrapping-i64"
            or overflow.get("status") != "requires-executable-negative-tests"
            or [v.get("id") for v in overflow.get("cases", [])] != [
                "positive-add-overflow", "negative-sub-overflow",
                "positive-multiply-overflow", "negative-unary-overflow"]):
        raise AssertionError("invalid shared overflow corpus")
    for case in overflow["cases"]:
        if case.get("expected") != "reject-or-runtime-error":
            raise AssertionError("unexpected shared overflow policy")
    with tempfile.TemporaryDirectory(prefix="ores-numeric-i64-") as temp:
        folder = Path(temp)
        for case in corpus["cases"]:
            check_positive(compiler, clang, case["expression"],
                           int(case["expected"]), folder, case["id"])
        negative_sources = [
            "fnc main():int{return 9223372036854775808;}",
            "fnc main():int{return -9223372036854775809;}",
            "fnc main():int{return 3/2;}",
            "fnc main():bigint{return 1;}",
            "fnc main():decimal{return 1;}",
            "fnc main():int{return 1.25;}",
        ]
        negative_sources += [
            "fnc main():int{return " + c["source"] + ";}"
            for c in overflow["cases"]
        ]
        for code in negative_sources:
            path = folder / "unsupported.ores"
            path.write_text(code, encoding="utf-8")
            negative = subprocess.run([compiler, "--target=host", str(path)],
                                      text=True, capture_output=True, timeout=20, check=False)
            if (negative.returncode != 1 or negative.stdout.strip()
                    or not re.search(r"^oreslang-llvmc:\s+\S+", negative.stderr, re.M)):
                raise AssertionError("unsupported precision source was not diagnosed by frontend")
    print("NATIVE_NUMERIC_I64_POSITIVE=7 REJECTED=10; dynamic arithmetic still unverified")


if __name__ == "__main__":
    main()
