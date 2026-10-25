#!/usr/bin/env python3
"""Exact native AOT guest corpus; exits nonzero on missing execution or mismatch.

This runs real Oreslang native compiler -> LLVM IR -> clang -> executable.
The fixture is a pinned copy of the shared semantic oracle, not identical
Oreslang source between Java and LLVM. Reports are NOT CI attestations.
"""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ORIGIN_GIT_BLOB = "58d2b64a137ede4700593f855e1f0504ff744f90"
ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "tests/fixtures/formal-core-scalar-checked.ores"
CASES = ROOT / "tests/fixtures/core-math-i64-v1.json"
MIN_I64, MAX_I64 = -(1 << 63), (1 << 63) - 1
DECIMAL = re.compile(r"(?:0|-?[1-9][0-9]*)\Z")
VALID_FUNCTIONS = {"min_int": 2, "max_int": 2, "clamp_int": 3}


def git_blob(data: bytes) -> str:
    header = f"blob {len(data)}\0".encode("ascii")
    return hashlib.sha1(header + data).hexdigest()


def no_duplicate_keys(pairs):
    result = {}
    for name, value in pairs:
        if name in result:
            raise ValueError("duplicate JSON key")
        result[name] = value
    return result


def int64(text):
    if not isinstance(text, str) or not DECIMAL.fullmatch(text):
        raise ValueError("invalid canonical int64 decimal")
    number = int(text)
    if number < MIN_I64 or number > MAX_I64:
        raise ValueError("outside signed 64-bit domain")
    return number


def oracle(name, args):
    if name == "min_int":
        return min(args)
    if name == "max_int":
        return max(args)
    if name == "clamp_int":
        value, low, high = args
        return low if low > high else min(max(value, low), high)
    raise ValueError("unknown oracle")


def read_cases():
    blob = CASES.read_bytes()
    if git_blob(blob) != ORIGIN_GIT_BLOB:
        raise ValueError("shared contract snapshot differs from reviewed Git blob")
    value = json.loads(blob.decode("utf8"), object_pairs_hook=no_duplicate_keys,
                       parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite JSON")))
    if set(value) != {"schemaVersion", "contract", "integerDomain", "statement",
                      "sourceOfTruth", "cases"}:
        raise ValueError("unknown or missing manifest members")
    if (value["schemaVersion"] != 1
            or value["contract"] != "ores-core-math-signed-i64-v1"
            or value["integerDomain"] != "signed-i64"
            or not isinstance(value["cases"], list)
            or len(value["cases"]) != 24):
        raise ValueError("unsupported corpus")
    seen = set()
    prepared = []
    for case in value["cases"]:
        if not isinstance(case, dict) or set(case) != {"id", "function", "args", "expected"}:
            raise ValueError("invalid case")
        case_id, fn, raw_args = case["id"], case["function"], case["args"]
        if (not isinstance(case_id, str)
                or not re.fullmatch(r"[a-z][a-z0-9-]{1,80}", case_id)
                or case_id in seen or fn not in VALID_FUNCTIONS
                or not isinstance(raw_args, list)
                or len(raw_args) != VALID_FUNCTIONS[fn]):
            raise ValueError("invalid function, arity or duplicate ID")
        seen.add(case_id)
        values = [int64(x) for x in raw_args]
        expected = int64(case["expected"])
        if oracle(fn, values) != expected:
            raise ValueError("independent oracle differs on " + case_id)
        prepared.append((case_id, fn, values, expected))
    for must in ("min-int64-lowest", "max-int64-highest",
                 "clamp-inverted-returns-lower", "clamp-all-extreme",
                 "min-large-precise"):
        if must not in seen:
            raise ValueError("missing edge case " + must)
    return prepared


def must_run(argv, *, timeout=60, cwd=None):
    result = subprocess.run(argv, capture_output=True, timeout=timeout, cwd=cwd,
                            check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"command failure ({result.returncode}) {Path(argv[0]).name}: "
            + result.stderr.decode("utf8", "replace")[-2000:])
    return result.stdout


def render_i64_literal(value):
    """Represent every signed-i64 input in the *current* native source grammar.

    Native Parser.cpp currently parses unary '-' separately from the positive
    integer token, and std::from_chars rejects the positive magnitude 2^63.
    This source-level expression uses only representable intermediates:
    -INT64_MAX - 1 == INT64_MIN, without overflow at either operation.
    """
    if type(value) is not int or value < MIN_I64 or value > MAX_I64:
        raise ValueError("guest literal outside exact signed-i64 domain")
    if value == MIN_I64:
        return "(-9223372036854775807 - 1)"
    return str(value)


def guest_program(source, fn, args, expected):
    if fn not in VALID_FUNCTIONS or len(args) != VALID_FUNCTIONS[fn]:
        raise ValueError("unreviewed guest function or arity")
    call = fn + "(" + ", ".join(map(render_i64_literal, args)) + ")"
    main = ("\nfnc main(): int {\n"
            + "  const actual: int = " + call + ";\n"
            + "  if (actual eq " + render_i64_literal(expected) + ") "
            + "{ return 0; } fi\n"
            + "  return 1;\n}\n")
    return source + main


def test_guest_literal_renderer():
    cases = read_cases()
    for case_id, fn, args, expected in cases:
        source = guest_program("", fn, args, expected)
        if "9223372036854775808" in source:
            raise AssertionError("native parser cannot parse i64-min positive magnitude")
        if not source.endswith("return 1;\n}\n"):
            raise AssertionError("guest fixture missing final fail-closed return")
    for value in (MIN_I64, MIN_I64 + 1, -1, 0, 1, MAX_I64):
        actual = render_i64_literal(value)
        if value == MIN_I64 and actual != "(-9223372036854775807 - 1)":
            raise AssertionError("i64-min does not use safe native expression")
        if value != MIN_I64 and actual != str(value):
            raise AssertionError("non-min signed-i64 changed")
    for invalid in (True, None, "-1", MIN_I64 - 1, MAX_I64 + 1):
        try:
            render_i64_literal(invalid)
        except ValueError:
            pass
        else:
            raise AssertionError("out-of-domain guest scalar literal accepted")
    print("NATIVE-I64-LITERAL-RENDER-OK 24 cases + boundaries and negatives")


def one_execution(compiler, clang, source, fn, args, expected, working, label, level):
    # An Oreslang comparison is checked in the *guest*; only its Boolean 0/1
    # result is returned as process status, avoiding i64 exit-code truncation.
    source_path = working / (label + ".ores")
    ll_path = working / (label + ".ll")
    exe_path = working / (label + "-" + level + ".exe")
    source_path.write_text(guest_program(source, fn, args, expected), encoding="ascii")
    ll_path.write_bytes(must_run([compiler, "--target=host", str(source_path)]))
    if level not in ("O0", "O2"):
        raise ValueError("unknown optimization level")
    must_run([clang, "-"+level, "-x", "ir", str(ll_path), "-o", str(exe_path)])
    p = subprocess.run([str(exe_path)], capture_output=True, timeout=10,
                       cwd=working, check=False)
    if p.returncode < 0:
        raise RuntimeError("compiled guest terminated by signal")
    if p.stdout or p.stderr:
        raise RuntimeError("unexpected native scalar proof guest I/O")
    return p.returncode


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--evidence-dir", required=True)
    options = parser.parse_args()
    cases = read_cases()
    source_blob = FIXTURE.read_bytes()
    source = source_blob.decode("ascii")
    if len(source_blob) > 200000 or "\0" in source:
        raise ValueError("invalid native source fixture")
    compiler = str(Path(options.compiler).resolve(strict=True))
    clang = str(Path(options.clang).resolve(strict=True))
    evidence = Path(options.evidence_dir).resolve()
    evidence.mkdir(parents=True, exist_ok=True)

    results = []
    with tempfile.TemporaryDirectory(prefix="native-ores-core-") as tmp:
        working = Path(tmp)
        for case_id, fn, args, expected in cases:
            exits = {}
            for level in ("O0", "O2"):
                exits[level] = one_execution(compiler, clang, source, fn, args,
                                             expected, working, case_id, level)
                if exits[level] != 0:
                    raise AssertionError(
                        f"compiled Oreslang result differs: {case_id} {level}")
            results.append({"id": case_id, "expected": str(expected),
                            "guest_exit_by_optimization": exits})
        # Deliberately wrong expectation must be rejected by the *compiled*
        # guest rather than by the Python oracle.
        fn, args, expected = cases[2][1:]
        wrong = expected + 1 if expected < MAX_I64 else expected - 1
        mutation_exits = {}
        for level in ("O0", "O2"):
            mutation_exits[level] = one_execution(compiler, clang, source, fn, args,
                                                  wrong, working, "wrong-expected", level)
            if mutation_exits[level] != 1:
                raise AssertionError(
                    f"compiled negative control did not fail under {level}")

    report = {
        "contract_git_blob": ORIGIN_GIT_BLOB,
        "native_fixture_git_blob": git_blob(source_blob),
        "backend": "llvm-native-host",
        "kind": "actual-compiled-guest",
        "cases": results,
        "negative_control_exit_by_optimization": mutation_exits,
        "executed": len(results) * 2,
    }
    (evidence / "aot-core-math-execution.json").write_text(
        json.dumps(report, sort_keys=True, indent=2) + "\n", encoding="utf8")
    print(f"NATIVE-AOT-CORE-MATH-OK {len(results)} cases x 2 optimization levels + 2 negative")


if __name__ == "__main__":
    try:
        if sys.argv[1:] == ["--self-test-literals"]:
            test_guest_literal_renderer()
        else:
            main()
    except Exception as error:
        print("NATIVE-AOT-CORE-MATH-UNVERIFIED: " + str(error), file=sys.stderr)
        sys.exit(1)
