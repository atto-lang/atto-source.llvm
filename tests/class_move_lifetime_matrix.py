#!/usr/bin/env python3
"""Prove native i64-record moves end the source lifetime exactly once.

All class fields are trivially destructible. This test deliberately does not
assert user-defined destructor/unwind/async semantics.
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def run(argv, **kwargs):
    return subprocess.run(argv, capture_output=True, text=True, timeout=40, **kwargs)


def source_for(width, hops, taken):
    fields = "\n".join(
        f"  pub let int f{i} = {42 if i == 0 else 0};" for i in range(width)
    )
    transfers = "\n".join(
        f"  let Shape p{i} = rt take p{i-1};" for i in range(1, hops + 1)
    )
    guard = 42 if taken else 43
    return (
        "define class Shape as\n" + fields + "\nend\n"
        "pub fnc main(): int {\n"
        "  let Shape p0 = new Shape();\n"
        + transfers + "\n"
        + f"  if (p{hops}.f0 eq {guard}) {{ return p{hops}.f0; }} fi\n"
        + f"  return p{hops}.f0;\n"
        + "}\n"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    count = 0
    with tempfile.TemporaryDirectory(prefix="ores-class-move-") as temp:
        for width in (1, 2, 4, 8, 16):
            for hops in (1, 2, 3):
                for taken in (True, False):
                    source = Path(temp) / f"w{width}-h{hops}-t{int(taken)}.ores"
                    executable = source.with_suffix(".guest")
                    source.write_text(source_for(width, hops, taken))
                    compiled = run([args.compiler, str(source)])
                    assert compiled.returncode == 0, (source.name, compiled.stderr)
                    ir = compiled.stdout
                    assert ir.startswith("; ModuleID"), (source.name, ir[:400])
                    assert "oreslang_owned_i64_region_" not in ir, source.name
                    drops = len(re.findall(r"call void @llvm\.lifetime\.end", ir))
                    # Each moved-from record ends at transfer (hops calls);
                    # destination is ended once per emitted return edge.
                    assert drops == hops + 2, (source.name, drops, hops + 2)
                    flags = (
                        ["-O1", "-g", "-fno-omit-frame-pointer", "-fsanitize=address,undefined"]
                        if args.sanitize else ["-O1"]
                    )
                    native = run([args.clang, *flags, "-x", "ir", "-", "-o", str(executable)],
                                 input=ir)
                    assert native.returncode == 0, (source.name, native.stderr)
                    guest = run([str(executable)])
                    assert guest.returncode == 42, (source.name, guest.returncode, guest.stderr)
                    count += 1
    print(f"CLASS_MOVE_LIFETIME_NATIVE_PASS={count} sanitized={args.sanitize}")


if __name__ == "__main__":
    main()
