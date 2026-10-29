#!/usr/bin/env python3
"""Real .ores -> LLVM IR -> Clang -> native process proof for stack-only structs.

No native aggregate parameter/return ABI, readonly-record construction, mutable
aggregate access, or string-field support is certified by these tests.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile


POSITIVE = (
    ("single-field-positional", """
pub fnc main(): int {
    type T = struct { foo: int };
    const T value = T{42};
    return value.foo;
}
""", ()),
    ("named-source-order", """
fnc mark_two(int x): int { return x; }
fnc mark_one(int x): int { return x; }
pub fnc main(): int {
    type T = struct { foo: int, bar: int };
    const T value = T{bar: mark_two(2), foo: mark_one(40)};
    return value.foo + value.bar;
}
""", (b"mark_two", b"mark_one")),
    ("bool-field", """
fnc ready(bool value): int { return value ? 42 : 0; }
pub fnc main(): int {
    type T = struct { active: bool };
    const T value = T{active: true};
    return ready(value.active);
}
""", ()),
    ("reordered-declaration", """
pub fnc main(): int {
    type T = struct { z: int, a: int };
    const T value = T{z: 2, a: 40};
    return value.a + value.z;
}
""", ()),
)

NEGATIVE = (
    ("dynamic-wrong-field-type", """
fnc get_flag(): bool { return true; }
pub fnc main(): int {
    type T = struct { foo: int };
    const T value = T{foo: get_flag()};
    return value.foo;
}
""", "AGGREGATE_CONSTRUCTOR_FIELD_TYPE_MISMATCH"),
    ("aggregate-as-i64", """
pub fnc main(): int {
    type T = struct { foo: int };
    const T value = T{foo: 42};
    return value;
}
""", "AGGREGATE_RETURN_TYPE_MISMATCH"),
    ("unknown-field", """
pub fnc main(): int {
    type T = struct { foo: int };
    const T value = T{foo: 42};
    return value.bar;
}
""", "AGGREGATE_FIELD_TYPE_UNVERIFIED"),
    ("unsupported-string-field", """
pub fnc main(): int {
    type T = struct { foo: string };
    const T value = T{foo: "hello"};
    return 42;
}
""", "AGGREGATE_NATIVE_FIELD_UNVERIFIED"),
)


def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def run(args, cwd):
    return subprocess.run(args, cwd=cwd, stdin=subprocess.DEVNULL,
                          capture_output=True, timeout=30, check=False)


def sha256(raw):
    return hashlib.sha256(raw).hexdigest()


def verify_negative_rejection(result, diagnostic):
    """Reject lookalike identifiers, published IR, and abnormal termination.

    A substring match would falsely admit
    AGGREGATE_FIELD_TYPE_UNVERIFIED_WRONG as the expected diagnostic.
    """
    token = rb"(?<![A-Za-z0-9_])" + re.escape(diagnostic.encode("ascii")) + rb"(?![A-Za-z0-9_])"
    require(type(result.returncode) is int and result.returncode > 0,
            "compiler did not reject source normally")
    require(result.stdout == b"", "rejected source emitted output (possible LLVM IR)")
    require(re.search(token, result.stderr) is not None,
            "expected diagnostic not found as complete identifier: " + diagnostic)
    for marker in (b"; ModuleID", b"source_filename =", b"target triple =", b"define "):
        require(marker not in result.stderr, "rejected source leaked LLVM IR in stderr")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--clang", type=Path, required=True)
    args = parser.parse_args()
    for binary in (args.compiler, args.clang):
        require(binary.exists() and binary.is_file(),
                "required exact-head compiler or Clang missing: " + str(binary))
    compiler, clang = args.compiler.resolve(), args.clang.resolve()
    compiler_digest, clang_digest = (
        sha256(compiler.read_bytes()), sha256(clang.read_bytes()))
    positive_names = [name for name, _, _ in POSITIVE]
    negative_names = [name for name, _, _ in NEGATIVE]
    require(len(positive_names) == 4 and len(negative_names) == 4 and
            len(set(positive_names + negative_names)) == 8,
            "aggregate executable acceptance requires four distinct positives and four distinct negatives")
    observations = []
    with tempfile.TemporaryDirectory(prefix="oreslang-native-aggregate-") as tmp:
        root = Path(tmp)
        for name, source, calls in POSITIVE:
            case_dir = root / name
            case_dir.mkdir()
            src = case_dir / "case.ores"
            ir = case_dir / "case.ll"
            exe = case_dir / "case"
            src.write_text(source, encoding="utf-8")
            original = src.read_bytes()
            compilation = run([str(compiler), str(src)], case_dir)
            require(compilation.returncode == 0 and compilation.stderr == b"" and
                    b"define i64 @__oreslang_guest_main(" in compilation.stdout and
                    b"define i32 @main(" in compilation.stdout,
                    name + ": real Oreslang compiler failed: " +
                    repr(compilation.stderr[:800]))
            require(set(p.name for p in case_dir.iterdir()) == {"case.ores"} and
                    src.read_bytes() == original and not src.is_symlink(),
                    name + ": compiler mutated source or emitted unexpected artifacts")
            if calls:
                matches = [re.search(rb"call\s+i64\s+@" + re.escape(call) + rb"\s*\(",
                                     compilation.stdout) for call in calls]
                require(all(matches) and matches[0].start() < matches[1].start(),
                        name + ": lexical initializer call order lost in unoptimized LLVM IR")
            ir.write_bytes(compilation.stdout)
            linked = run([str(clang), "-O0", "-x", "ir", str(ir), "-o", str(exe)], case_dir)
            require(linked.returncode == 0 and exe.is_file(),
                    name + ": Clang failed to link actual guest IR: " +
                    repr(linked.stderr[:800]))
            observed = run([str(exe)], case_dir)
            require(observed.returncode == 42 and observed.stdout == b"" and
                    observed.stderr == b"",
                    name + ": expected OS exit 42: " +
                    repr((observed.returncode, observed.stdout, observed.stderr)))
            observations.append({
                "case": name, "os_exit": observed.returncode,
                "source_sha256": sha256(original),
                "ir_sha256": sha256(compilation.stdout),
                "binary_sha256": sha256(exe.read_bytes())
            })
        for name, source, diagnostic in NEGATIVE:
            case_dir = root / name
            case_dir.mkdir()
            src = case_dir / "case.ores"
            src.write_text(source, encoding="utf-8")
            original = src.read_bytes()
            rejected = run([str(compiler), str(src)], case_dir)
            try:
                verify_negative_rejection(rejected, diagnostic)
            except RuntimeError as exc:
                raise RuntimeError(
                    name + ": missing exact pre-IR rejection: " + str(exc) +
                    "; result=" + repr((rejected.returncode,
                                        rejected.stdout[:300],
                                        rejected.stderr[:700]))) from exc
            require(set(p.name for p in case_dir.iterdir()) == {"case.ores"} and
                    src.read_bytes() == original and not src.is_symlink(),
                    name + ": negative compiler run published files or mutated source")
    require(sha256(compiler.read_bytes()) == compiler_digest and
            sha256(clang.read_bytes()) == clang_digest,
            "compiler or Clang binary changed while guest tests executed")
    print(json.dumps({
        "contract": "native-stack-exact-struct-v1",
        "scope": "immutable-int-bool-local-no-aggregate-call-abi",
        "compiler_sha256": compiler_digest,
        "clang_sha256": clang_digest,
        "positive_executed": len(observations),
        "negative_rejected": len(NEGATIVE),
        "skipped": 0,
        "observations": observations
    }, sort_keys=True))


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("native aggregate executable acceptance DENIED: " + str(exc),
              file=sys.stderr)
        sys.exit(1)
