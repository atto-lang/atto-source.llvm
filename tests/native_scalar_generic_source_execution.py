#!/usr/bin/env python3
"""Actual native Oreslang -> LLVM IR -> Clang guest i64 generic conformance.

Stage 1 intentionally admits only generics whose concrete representation is
int64. This test never treats unsupported classes/actors as generic support.
"""
import argparse
import json
import re
from pathlib import Path
import subprocess
import tempfile

VALID = {
    "explicit-scalar": ("""fnc identity<T>(T value): T { return value; }
pub fnc main(): int { return identity<int>(42); }
""", 42),
    "inferred-scalar": ("""fnc identity<T>(T value): T { return value; }
pub fnc main(): int { return identity(42); }
""", 42),
    "dependent-default": ("""fnc add<A, B=int, C=B>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<int>(19, 23); }
""", 42),
    "named-override": ("""fnc add<A, B=int, C=B>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<C=int, A=int>(19, 23); }
""", 42),
    "inferred-explicit-hole": ("""fnc identity<T=int>(T value): T { return value; }
pub fnc main(): int { return identity<T=_>(42); }
""", 42),
    "default-omission": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed(); }
""", 42),
    "dependent-chain": ("""fnc sum<A, B=A, C=B, D=C>(A x, B y): int { return x + y; }
pub fnc main(): int { return sum<A=int, D=int>(19, 23); }
""", 42),
    "positional-plus-named": ("""fnc add<A, B=int, C=B>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<int, C=int>(19, 23); }
""", 42),
    "inferred-hole-with-other-default": ("""fnc identity<A, B=int>(A value): A { return value; }
pub fnc main(): int { return identity<_, B=int>(42); }
""", 42),
}
INVALID = {
    "missing-required": ("""fnc fixed<T>(): int { return 42; }
pub fnc main(): int { return fixed(); }
""", "cannot infer generic parameter"),
    "uninferable-hole": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<_>(); }
""", "cannot infer generic parameter"),
    "nested-nonscalar": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Array<int>>(); }
""", "native scalar generic type requires int"),
    "unknown-type": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<bool>(); }
""", "native scalar generic type requires int"),
    "duplicate-named": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<T=int, T=int>(); }
""", "too many generic type arguments"),
    "unknown-named": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Other=int>(); }
""", "unknown named generic argument"),
    "positional-after-named": ("""fnc add<A, B=int>(A x, B y): int { return x + y; }
pub fnc main(): int { return add<B=int, int>(19, 23); }
""", "positional generic argument after named argument"),
    "forward-default": ("""fnc invalid<A=B, B=int>(): int { return 42; }
pub fnc main(): int { return invalid<int>(); }
""", "forward or self-referential generic default"),
    "generic-main": ("""pub fnc main<T=int>(): int { return 42; }
""", "native entrypoint main cannot be generic"),
    "bad-nested-hole": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<Option<_>>(); }
""", "native scalar generic type requires int"),
    "dependent-hole-cannot-default": ("""fnc fixed<A=int, B=A>(): int { return 42; }
pub fnc main(): int { return fixed<A=_>(); }
""", "cannot infer generic parameter"),
    "unknown-named-override": ("""fnc fixed<A, B=int>(): int { return 42; }
pub fnc main(): int { return fixed<C=int>(); }
""", "unknown named generic argument"),
    "duplicate-generic-declaration": ("""fnc fixed<A, A=int>(): int { return 42; }
pub fnc main(): int { return fixed<int>(); }
""", "duplicate generic parameter"),
    "self-dependent-default": ("""fnc fixed<A=A>(): int { return 42; }
pub fnc main(): int { return fixed(); }
""", "forward or self-referential generic default"),
}

_CASE_ID = re.compile(r"generics\.scalar\.[a-z0-9]+(?:-[a-z0-9]+)*\Z")

def load_shared_conformance(contract=None):
    if contract is None:
        contract = Path(__file__).resolve().parents[1] / "contracts/v1/generics-stage1/cases.json"
    contract = Path(contract)
    if contract.stat().st_size > 1_048_576:
        raise AssertionError("generic conformance contract exceeds 1 MiB")
    payload = json.loads(contract.read_text(encoding="utf-8"))
    if not isinstance(payload, dict) or payload.get("schema_version") != "1.0.0":
        raise AssertionError("unsupported generic conformance contract schema")
    if payload.get("scope") != "native-scalar-i64-only":
        raise AssertionError("unsupported generic conformance scope")
    cases = payload.get("cases")
    if not isinstance(cases, list) or not 1 <= len(cases) <= 256:
        raise AssertionError("generic conformance cases must be a nonempty bounded list")
    seen = set(VALID) | set(INVALID)
    valid, invalid = {}, {}
    for case in cases:
        if not isinstance(case, dict):
            raise AssertionError("generic conformance case must be an object")
        identifier = case.get("id")
        if not isinstance(identifier, str) or not _CASE_ID.fullmatch(identifier):
            raise AssertionError(f"invalid generic conformance ID: {identifier!r}")
        if identifier in seen:
            raise AssertionError(f"duplicate or colliding generic conformance ID: {identifier}")
        seen.add(identifier)
        source = case.get("source")
        if not isinstance(source, str) or not source.strip() or len(source) > 65_536 or "\x00" in source:
            raise AssertionError(f"{identifier}: invalid or oversized generic source")
        kind = case.get("kind")
        if kind == "native_exit":
            if set(case) != {"id", "kind", "source", "expected_exit"}:
                raise AssertionError(f"{identifier}: invalid native_exit fields")
            status = case["expected_exit"]
            if type(status) is not int or not 0 <= status <= 255:
                raise AssertionError(f"{identifier}: exit code must be an integer in 0..255")
            valid[identifier] = (source, status)
        elif kind == "diagnostic":
            if set(case) != {"id", "kind", "source", "expected_substring"}:
                raise AssertionError(f"{identifier}: invalid diagnostic fields")
            diagnostic = case["expected_substring"]
            if not isinstance(diagnostic, str) or not diagnostic.strip() or len(diagnostic) > 256 or "\x00" in diagnostic:
                raise AssertionError(f"{identifier}: diagnostic must be a nonempty bounded string")
            invalid[identifier] = (source, diagnostic)
        else:
            raise AssertionError(f"unsupported generic conformance case {identifier}")
    return valid, invalid


def run(argv):
    return subprocess.run(argv, capture_output=True, timeout=65, check=False)

def verify(compiler, clang):
    with tempfile.TemporaryDirectory(prefix="oreslang-native-generics-") as tmp:
        tmp = Path(tmp)
        shared_valid, shared_invalid = load_shared_conformance()
        for name, (source, exit_code) in {**VALID, **shared_valid}.items():
            path = tmp / (name + ".ores")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode or not result.stdout.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={result.returncode}, stderr={result.stderr!r}")
            ll = tmp / (name + ".ll")
            exe = tmp / name
            ll.write_bytes(result.stdout)
            linking = run([str(clang), "-Wno-override-module", "-x", "ir", str(ll), "-o", str(exe)])
            if linking.returncode:
                raise AssertionError(f"{name}: Clang failed: {linking.stderr!r}")
            executed = run([str(exe)])
            if executed.returncode != exit_code:
                raise AssertionError(f"{name}: guest exit {executed.returncode}, expected {exit_code}, stderr={executed.stderr!r}")
            print(f"PASS guest {name}: {exit_code}", flush=True)
        for name, (source, diagnostic) in {**INVALID, **shared_invalid}.items():
            path = tmp / (name + ".ores")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode == 0 or result.returncode < 0 or result.stdout:
                raise AssertionError(f"{name}: compiler must reject before IR, rc={result.returncode}")
            if diagnostic.encode() not in result.stderr:
                raise AssertionError(f"{name}: expected {diagnostic!r}, got {result.stderr!r}")
            print(f"PASS reject {name}: {diagnostic}", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    verify(args.compiler.resolve(), args.clang.resolve())
