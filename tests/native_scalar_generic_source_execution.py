#!/usr/bin/env python3
"""Historical native LLVM scalar compiler regressions (non-normative).

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
    # Joint name binding must NOT reorder lexical expressions or ABI slots.
    "generic-and-value-reordered": ("""fnc combine<A=int, B=A>(A first, B second): int { return first * 10 + second; }
pub fnc main(): int { return combine<B=int, A=int>(second: 2, first: 4); }
""", 42),
    "positional-generic-and-named-value": ("""fnc add<T=int>(T first, T second): int { return first + second; }
pub fnc main(): int { return add<int>(19, second: 23); }
""", 42),
    "inferred-generic-and-reordered-value": ("""fnc add<T>(T first, T second): int { return first + second; }
pub fnc main(): int { return add(second: 23, first: 19); }
""", 42),
    "named-reordered-nested-calls": ("""fnc four(): int { return 4; }
fnc two(): int { return 2; }
fnc combine<A=int, B=A>(A first, B second): int { return first * 10 + second; }
pub fnc main(): int { return combine<B=int, A=int>(second: two(), first: four()); }
""", 42),
    "routine-named-value-reorder": ("""routine encode(int first, int second): int {
    return first * 10 + second;
}
pub fnc main(): int { return encode(second: 2, first: 4); }
""", 42),
    # Generic instance methods reuse checked scalar direct-call
    # specialization after the receiver becomes an owned/reborrowed self slot.
    "generic-instance-explicit": ("""define class Box as
  let int n=42;
  pub identity<T=int>(T value): int { return value; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.identity<T=int>(42);
}
""", 42),
    "generic-instance-inferred": ("""define class Box as
  let int n=42;
  pub identity<T>(T value): int { return value; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.identity(42);
}
""", 42),
    "generic-instance-named-reordered": ("""define class Box as
  let int n=42;
  pub combine<A=int, B=A>(A first, B second): int { return first * 10 + second; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.combine<B=int, A=int>(second: 2, first: 4);
}
""", 42),
    "generic-instance-mixed-arguments": ("""define class Box as
  let int n=42;
  pub add<T=int>(T first, T second): int { return first + second; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.add<T=int>(19, second: 23);
}
""", 42),
    "generic-instance-dependent-default": ("""define class Box as
  let int n=42;
  pub value<A=int, B=A>(A first, B second): int { return first + second; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.value<A=int>(second: 23, first: 19);
}
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
    "duplicate-named": ("""fnc fixed<A=int, B=int>(): int { return 42; }
pub fnc main(): int { return fixed<A=int, A=int>(); }
""", "duplicate generic argument 'A'"),
    "positional-and-named-same-generic-slot": ("""fnc fixed<A=int, B=int>(): int { return 42; }
pub fnc main(): int { return fixed<int, A=int>(); }
""", "duplicate generic argument 'A'"),
    "case-sensitive-generic-name": ("""fnc fixed<A=int, B=int>(): int { return 42; }
pub fnc main(): int { return fixed<a=int, B=int>(); }
""", "unknown named generic argument 'a'"),
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
    "duplicate-named-value-with-generics": ("""fnc add<T>(T first, T second): int { return first + second; }
pub fnc main(): int { return add<T=int>(first: 19, first: 23); }
""", "duplicate named argument"),
    # The binder must reject the *late* label before recursing into the
    # invalid first argument; otherwise diagnostics depend on source effects.
    "unknown-named-value-with-generics": ("""fnc add<T>(T first, T second): int { return first + second; }
pub fnc main(): int { return add<T=int>(first: doesNotExist(), unknown: 23); }
""", "unknown named argument"),
    "positional-after-named-value-with-generics": ("""fnc add<T>(T first, T second): int { return first + second; }
pub fnc main(): int { return add<T=int>(second: 23, 19); }
""", "positional argument cannot follow named argument"),
    "nonscalar-named-vector": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<T=Vector<int>>(); }
""", "native scalar generic type requires int"),
    "nonscalar-named-arraylist": ("""fnc fixed<T=int>(): int { return 42; }
pub fnc main(): int { return fixed<T=ArrayList<int>>(); }
""", "native scalar generic type requires int"),
    "overfull-named-value-call": ("""fnc add(int first, int second): int { return first + second; }
pub fnc main(): int { return add(first: 19, second: 23, extra: 1); }
""", "unknown function or wrong call arity"),
    "duplicate-named-array-set": ("""pub fnc main(): int {
    let mut Array<int> values = new Array<int>();
    values.set(index: 0, index: 1);
    return 42;
}
""", "duplicate named member argument"),
    "positional-after-named-array-set": ("""pub fnc main(): int {
    let mut Array<int> values = new Array<int>();
    values.set(index: 0, 42);
    return 42;
}
""", "positional member argument cannot follow named argument"),
    "case-sensitive-value-label": ("""fnc combine<T=int>(T first, T second): int { return first * 10 + second; }
pub fnc main(): int { return combine<T=int>(First: 4, second: 2); }
""", "unknown named argument"),
    "array-unknown-named-get": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(42);
  return values.get(offset: 0);
}
""", "unknown named native array argument 'offset'"),
    "array-unknown-named-set": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(offset: 0, value: 42);
  return 42;
}
""", "unknown named native array argument 'offset'"),
    "array-case-sensitive-named-get": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(42);
  return values.get(Index: 0);
}
""", "unknown named native array argument 'Index'"),
    "array-positional-slot-collision": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(0, index: 1);
  return 42;
}
""", "duplicate native array argument binding"),
    "array-missing-named-set-slot": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(value: 42);
  return 42;
}
""", "native Array method argument arity mismatch"),
    "array-excess-named-set-slot": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(index: 0, value: 42, extra: 9);
  return 42;
}
""", "native Array method argument arity mismatch"),
    "array-wrong-named-value-type": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(value: true, index: 0);
  return 42;
}
""", "native Array<int> arguments require int, got bool"),
    "array-wrong-named-index-type": ("""pub fnc main(): int {
  let mut Array<int> values = new Array<int>();
  values.add(0);
  values.set(value: 42, index: true);
  return 42;
}
""", "native Array<int> arguments require int, got bool"),
    "array-nonarray-named-receiver": ("""fnc bad(int value): int {
  return value.get(index: 0);
}
pub fnc main(): int { return bad(42); }
""", "native Array<int> operation requires a live array owner"),
    "constructor-late-unknown-before-affine-move": ("""define class Pair as
  pub final mut Array<int> left;
  pub final mut Array<int> right;
end
pub fnc main(): int {
  let mut Array<int> a = new Array<int>();
  let mut Array<int> b = new Array<int>();
  let Pair p = new Pair(left: rt take a, unknown: rt take b);
  return 42;
}
""", "unknown named native constructor argument 'unknown'"),
    "generic-method-nongeneric-invocation": ("""define class Box as
  let int n=42;
  pub value(int x): int { return x; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.value<T=int>(42);
}
""", "call-site type arguments require generic function"),
    "generic-method-unknown-type-name": ("""define class Box as
  let int n=42;
  pub id<T=int>(T x): int { return x; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<Missing=int>(42);
}
""", "unknown named generic argument"),
    "generic-method-type-duplicate": ("""define class Box as
  let int n=42;
  pub id<A=int, B=int>(A x): int { return x; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<A=int, A=int>(42);
}
""", "duplicate generic argument 'A'"),
    "generic-method-type-positional-after-named": ("""define class Box as
  let int n=42;
  pub id<A=int, B=int>(A x): int { return x; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<A=int, int>(42);
}
""", "positional generic argument after named argument"),
    "generic-method-unknown-value-label": ("""define class Box as
  let int n=42;
  pub id<T=int>(T value): int { return value; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<T=int>(unknown: 42);
}
""", "unknown named argument"),
    "generic-method-duplicate-value-label": ("""define class Box as
  let int n=42;
  pub id<T=int>(T first, T second): int { return first + second; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<T=int>(first: 19, first: 23);
}
""", "duplicate named member argument"),
    "generic-method-value-positional-after-named": ("""define class Box as
  let int n=42;
  pub id<T=int>(T first, T second): int { return first + second; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<T=int>(second: 23, 19);
}
""", "positional member argument cannot follow named argument"),
    "generic-method-bool-type-unsupported": ("""define class Box as
  let int n=42;
  pub id<T=int>(T value): int { return value; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.id<T=bool>(42);
}
""", "native scalar generic type requires int or bare '_'"),
    "generic-array-method-reject": ("""pub fnc main(): int {
  let mut Array<int> values=new Array<int>();
  values.add(42);
  return values.get<T=int>(index: 0);
}
""", "native Array methods do not accept generic type arguments"),
    "generic-method-private-access": ("""define class Box as
  let int n=42;
  hidden<T=int>(T value): int { return value; }
end
pub fnc main(): int {
  let Box b=new Box();
  return b.hidden<T=int>(42);
}
""", "private instance method is not accessible"),
}

_CASE_ID = re.compile(r"generics\.scalar\.[a-z0-9]+(?:-[a-z0-9]+)*\Z")
_MAX_CAPTURE = 2 * 1024 * 1024


def _unique_json_object(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise AssertionError(f"duplicate generic contract JSON key: {key}")
        obj[key] = value
    return obj


def _reject_json_constant(token):
    raise AssertionError(f"nonfinite JSON value in generic contract: {token}")


def load_shared_conformance(contract):
    """Validate an explicit synthetic LLVM regression contract only."""
    contract = Path(contract)
    if contract.stat().st_size > 1_048_576:
        raise AssertionError("generic conformance contract exceeds 1 MiB")
    payload = json.loads(
        contract.read_text(encoding="utf-8"),
        object_pairs_hook=_unique_json_object,
        parse_constant=_reject_json_constant,
    )
    if not isinstance(payload, dict):
        raise AssertionError("generic conformance contract must be an object")
    if set(payload) not in ({"schema_version", "scope", "cases"},
                            {"schema_version", "scope", "cases", "notes"}):
        raise AssertionError("unexpected generic contract top-level fields")
    if "notes" in payload and (not isinstance(payload["notes"], str)
                               or len(payload["notes"]) > 4096):
        raise AssertionError("invalid or oversized generic contract notes")
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
    # Disk-backed capture bounds Python memory even for misbehaving tools.
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        result = subprocess.run(argv, stdout=out, stderr=err,
                                timeout=65, check=False)
        out.seek(0, 2)
        err.seek(0, 2)
        if out.tell() > _MAX_CAPTURE or err.tell() > _MAX_CAPTURE:
            raise AssertionError("generic tool emitted oversized stdout/stderr")
        out.seek(0)
        err.seek(0)
        return subprocess.CompletedProcess(argv, result.returncode,
                                           out.read(), err.read())

def verify(compiler, clang, clang_opt):
    with tempfile.TemporaryDirectory(prefix="oreslang-native-generics-") as tmp:
        tmp = Path(tmp)
        for name, (source, exit_code) in VALID.items():
            path = tmp / (name + ".atto")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode or result.stderr or not result.stdout.startswith(b"; ModuleID"):
                raise AssertionError(f"{name}: compiler rc={result.returncode}, stderr={result.stderr!r}")
            ll = tmp / (name + ".ll")
            exe = tmp / name
            ll.write_bytes(result.stdout)
            linking = run([str(clang), clang_opt, "-Wno-override-module", "-x", "ir", str(ll), "-o", str(exe)])
            if linking.returncode:
                raise AssertionError(f"{name}: Clang failed: {linking.stderr!r}")
            executed = run([str(exe)])
            if executed.returncode != exit_code:
                raise AssertionError(f"{name}: guest exit {executed.returncode}, expected {exit_code}, stderr={executed.stderr!r}")
            if executed.stdout or executed.stderr:
                raise AssertionError(f"{name}: scalar guest unexpectedly emitted output: "
                                     f"stdout={executed.stdout!r}, stderr={executed.stderr!r}")
            print(f"PASS guest {name}: {exit_code}", flush=True)
        for name, (source, diagnostic) in INVALID.items():
            path = tmp / (name + ".atto")
            path.write_text(source)
            result = run([str(compiler), str(path)])
            if result.returncode == 0 or result.returncode < 0 or result.stdout:
                raise AssertionError(f"{name}: compiler must reject before IR, rc={result.returncode}")
            # Sanitizer failures are not valid expected compiler diagnostics.
            # Otherwise a frontend crash after printing the expected substring
            # could be mistakenly counted as a negative-test pass.
            sanitizer_signatures = (
                b"AddressSanitizer", b"UndefinedBehaviorSanitizer",
                b"LeakSanitizer", b"runtime error: signed integer overflow",
            )
            if any(signature in result.stderr for signature in sanitizer_signatures):
                raise AssertionError(f"{name}: native sanitizer error: {result.stderr!r}")
            if diagnostic.encode() not in result.stderr:
                raise AssertionError(f"{name}: expected {diagnostic!r}, got {result.stderr!r}")
            print(f"PASS reject {name}: {diagnostic}", flush=True)

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--clang-opt", choices=("-O0", "-O2"), default="-O0")
    args = parser.parse_args()
    verify(args.compiler.resolve(), args.clang.resolve(), args.clang_opt)
