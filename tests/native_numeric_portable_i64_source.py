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
ENTRY = re.compile(r"define\s+i64\s+@__oreslang_guest_main\s*\(\s*\)")
CALL = re.compile(r"@main(?=\s*\()")


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
    renamed = CALL.sub("@oreslang_os_main", ir)
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
    with tempfile.TemporaryDirectory(prefix="ores-numeric-i64-") as temp:
        folder = Path(temp)
        for case in corpus["cases"]:
            check_positive(compiler, clang, case["expression"],
                           int(case["expected"]), folder, case["id"])
        for code in (
            "fnc main():int{return 3/2;}",
            "fnc main():bigint{return 1;}",
            "fnc main():decimal{return 1;}",
            "fnc main():int{return 1.25;}",
        ):
            path = folder / "unsupported.ores"
            path.write_text(code, encoding="utf-8")
            negative = subprocess.run([compiler, "--target=host", str(path)],
                                      text=True, capture_output=True, timeout=20, check=False)
            if (negative.returncode != 1 or negative.stdout.strip()
                    or not re.search(r"^oreslang-llvmc:\s+\S+", negative.stderr, re.M)):
                raise AssertionError("unsupported precision source was not diagnosed by frontend")
    print("NATIVE_NUMERIC_I64_POSITIVE=7 NEGATIVE=4; advanced domains unsupported")


if __name__ == "__main__":
    main()
