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


def check_positive(compiler, clang, source, expected, folder, label,
                   full_source=False):
    file = folder / "case.ores"
    file.write_text(source if full_source else
                    "pub fnc main():int{return " + source + ";}\n",
                    encoding="utf-8")
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


# These assertions are deliberately required only for the compile-success
# arm of shared overflow tests. A generic SIGILL/SIGTRAP is not evidence of
# signed-i64 overflow: the actual compiler IR must connect an overflow-flag
# extraction to the conditional trap block before that signal can count.
_OVERFLOW_PAIR = re.compile(
    r"(?m)^\s*(%[A-Za-z_.$][A-Za-z_0-9.$]*)\s*=\s*call\s+\{\s*i64\s*,\s*i1\s*\}"
    r"\s+@llvm\.(sadd|ssub|smul)\.with\.overflow\.i64\s*\(")
_OVERFLOW_FLAG = re.compile(
    r"(?m)^\s*(%[A-Za-z_.$][A-Za-z_0-9.$]*)\s*=\s*extractvalue\s+\{\s*i64\s*,\s*i1\s*\}"
    r"\s+(%[A-Za-z_.$][A-Za-z_0-9.$]*)\s*,\s*1\b")
_OVERFLOW_GUARD = re.compile(
    r"(?m)^\s*br\s+i1\s+(%[A-Za-z_.$][A-Za-z_0-9.$]*)\s*,\s*label"
    r"\s+%(i64\.overflow\.trap(?:\.\d+)?)\s*,\s*label"
    r"\s+%i64\.checked\.continue(?:\.\d+)?\b")
_OVERFLOW_FAILURE = re.compile(
    r"(?m)^\s*(i64\.overflow\.trap(?:\.\d+)?):\s*(?:;[^\n]*)?\n"
    r"\s*call\s+void\s+@llvm\.trap\s*\(\s*\)\s*\n\s*unreachable\b")


def assert_checked_overflow_path(ir, operation):
    """Reject IR with an unguarded/unrelated trap masquerading as overflow.

    This is a conservative structure check of Oreslang's experimental pure
    scalar lowering, not a full LLVM verification or an actor cleanup proof.
    """
    if not isinstance(ir, str) or len(ir) > 2 * 1024 * 1024:
        raise AssertionError("invalid checked overflow IR")
    if operation not in ("sadd", "ssub", "smul"):
        raise AssertionError("unsupported checked overflow operation")
    # Validate the real entrypoint. A checked helper in an unrelated
    # function must not be accepted as proof that guest main overflows.
    entries = re.findall(
        r"(?ms)^define[ \\t]+i64[ \\t]+@__oreslang_guest_main[ \\t]*"
        r"\\([ \\t]*\\)[^\\n{]*\\{[^\\n]*\\n([\\s\\S]*?)^\\}[ \\t]*$", ir)
    if len(entries) != 1:
        raise AssertionError("overflow IR requires exactly one guest main body")
    body = entries[0]
    pairs = list(_OVERFLOW_PAIR.finditer(body))
    pair_ops = {x.group(1): x.group(2) for x in pairs}
    if operation not in pair_ops.values():
        raise AssertionError("signed-i64 overflow intrinsic absent: " + operation)
    flags = {x.group(1): x.group(2) for x in _OVERFLOW_FLAG.finditer(body)
             if x.group(2) in pair_ops}
    guards = list(_OVERFLOW_GUARD.finditer(body))
    traps = re.findall(r"\bcall\s+void\s+@llvm\.trap\s*\(\s*\)", body)
    failures = list(_OVERFLOW_FAILURE.finditer(body))
    labels = {x.group(1) for x in failures}
    unreachable = re.findall(r"(?m)^\s*unreachable\b", body)
    if (not flags or not guards or not traps
            or len(guards) != len(traps) or len(failures) != len(traps)
            or len(unreachable) != len(failures)
            or len({x.group(2) for x in guards}) != len(guards)
            or any(x.group(1) not in flags or x.group(2) not in labels
                   for x in guards)
            or not any(pair_ops[flags[x.group(1)]] == operation
                       for x in guards if x.group(1) in flags)):
        raise AssertionError("overflow IR lacks operation-specific flag-derived guarded trap")


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
    if corpus.get("schema_version") != "1.0.0" or len(corpus.get("cases", [])) != 7:
        raise AssertionError("invalid or incomplete native guest signed-i64 corpus")
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
        # The same exact file bytes are executed by the Java/Graal test and
        # by the LLVM-to-Wasm scalar parity gate. This test uses the native
        # compiler and a full-width i64 host launcher, not a Python oracle.
        shared = (ROOT / "contracts/v1/numeric-parity/i64-max-minus-one.ores").read_bytes()
        git_blob = hashlib.sha1(
            b"blob " + str(len(shared)).encode("ascii") + b"\0" + shared
        ).hexdigest()
        if git_blob != "c400f440fde2fbc10514ab7cfd2ffe3d19729ee1":
            raise AssertionError("cross-backend exact i64 source fixture drift")
        check_positive(compiler, clang, shared.decode("utf-8"),
                       9223372036854775806, folder, "shared-max-minus-one",
                       full_source=True)
        unsupported_sources = [
            "fnc main():int{return 9223372036854775808;}",
            "fnc main():int{return -9223372036854775809;}",
            "fnc main():int{return 3/2;}",
            "fnc main():bigint{return 1;}",
            "fnc main():decimal{return 1;}",
            "fnc main():int{return 1.25;}",
        ]
        for code in unsupported_sources:
            path = folder / "unsupported.ores"
            path.write_text(code, encoding="utf-8")
            negative = subprocess.run([compiler, "--target=host", str(path)],
                                      text=True, capture_output=True, timeout=20, check=False)
            if (negative.returncode != 1 or negative.stdout.strip()
                    or not re.search(r"^oreslang-llvmc:\s+\S+", negative.stderr, re.M)):
                raise AssertionError("unsupported numeric syntax was not rejected by frontend")
        # The shared overflow contract permits a diagnosed compile-time
        # refusal OR an actual signed-overflow runtime trap, but NEVER a
        # successful/wrapped i64 result. Check which outcome occurred rather
        # than mistakenly requiring all four to fail during parsing.
        import signal
        expected_ops = {
            "positive-add-overflow": "sadd",
            "negative-sub-overflow": "ssub",
            "positive-multiply-overflow": "smul",
            "negative-unary-overflow": "ssub",
        }
        safe_neighbours = {
            "positive-add-overflow": ("(9223372036854775807-1)+1", 9223372036854775807),
            "negative-sub-overflow": ("(-9223372036854775808+1)-1", -9223372036854775808),
            "positive-multiply-overflow": ("3037000499*3037000499", 3037000499**2),
            "negative-unary-overflow": ("-(-9223372036854775808+1)", 9223372036854775807),
        }
        rejection_count = 0
        runtime_traps = 0
        for case in overflow["cases"]:
            path = folder / "checked-overflow.ores"
            path.write_text("pub fnc main():int{return " + case["source"] + ";}",
                            encoding="utf-8")
            attempt = subprocess.run([compiler, "--target=host", str(path)],
                                     text=True, capture_output=True, timeout=20, check=False)
            if attempt.returncode != 0:
                if (attempt.returncode != 1 or attempt.stdout.strip()
                        or not re.search(r"^oreslang-llvmc:\s+\S+", attempt.stderr, re.M)
                        or not re.search(r"overflow|out.of.range|out.of.bounds|invalid i64",
                                         attempt.stderr, re.I)):
                    raise AssertionError(case["id"] + ": unrelated compiler error is not an overflow pass")
                rejection_count += 1
                continue
            if attempt.stderr.strip() or not ENTRY.search(attempt.stdout):
                raise AssertionError(case["id"] + ": malformed compiler-success IR")
            assert_checked_overflow_path(attempt.stdout, expected_ops[case["id"]])
            ir, count = HOST_WRAPPER.subn(r"\1@oreslang_os_main", attempt.stdout)
            if count != 1:
                raise AssertionError(case["id"] + ": unexpected host wrapper inventory")
            object_file = folder / "checked-overflow.o"
            run([clang, "-x", "ir", "-c", "-o", str(object_file), "-"], input=ir)
            launcher = folder / "checked-overflow-host.c"
            launcher.write_text(
                '#include <stdint.h>\n'
                'extern int64_t __oreslang_guest_main(void);\n'
                'int main(void) { volatile int64_t value = __oreslang_guest_main();'
                ' (void)value; return 0; }\n', encoding="utf-8")
            executable = folder / "checked-overflow-host"
            run([clang, str(launcher), str(object_file), "-o", str(executable)])
            execution = subprocess.run([str(executable)], text=True,
                                       capture_output=True, timeout=10, check=False)
            if (execution.returncode not in (-signal.SIGILL, -signal.SIGTRAP)
                    or execution.stdout or execution.stderr):
                raise AssertionError(case["id"] +
                    ": emitted guest arithmetic did not trap; wrapped value or unrelated error")
            # A compiler that unconditionally traps even for valid nearby
            # arithmetic cannot satisfy signed-i64 numerical semantics.
            expression, expected = safe_neighbours[case["id"]]
            check_positive(compiler, clang, expression, expected, folder,
                           case["id"] + "-safe-boundary")
            runtime_traps += 1
    print(f"NATIVE_NUMERIC_I64_POSITIVE=8 UNSUPPORTED=6 OVERFLOW_REJECTED={rejection_count}"
          f" OVERFLOW_TRAPPED={runtime_traps}; dynamic arithmetic still unverified")


if __name__ == "__main__":
    main()
