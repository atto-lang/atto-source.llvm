#!/usr/bin/env python3
"""Native structural-row boundary: fail closed before any LLVM IR emission.

This gate proves rejection only. It does NOT imply LLVM record/struct lowering,
structural width subtyping, ownership-safe views, or actor transport parity.
"""
from __future__ import annotations

import argparse
import pathlib
import subprocess
import tempfile

CONTROL = "fnc main(): int { return 42; }\n"
CONTROL_COMMENTS = (
    "/* record{foo: int} structural {foo: int} "
    "/* struct{bar: int} */ */\n"
    "// fnc take(record{foo: int} x): int { return x.foo; }\n"
    "fnc main(): int { return 42; }\n"
)
REJECTED = {
    "record-param": "fnc take(record{foo: int} x): int { return x.foo; }\n",
    "struct-param": "fnc take(struct{foo: int} x): int { return x.foo; }\n",
    "structural-param": "fnc take(structural {foo: int} x): int { return x.foo; }\n",
    "named-structural": "fnc take(structural Shape x): int { return 42; }\n",
    "nested-record": "fnc take(List<record{foo: int}> x): int { return 42; }\n",
    "record-with-comment": (
        "fnc take(record /* compiler trivia */ {foo: int} x): int "
        "{ return 42; }\n"
    ),
    "inferred-struct": (
        "fnc main(): int { const x = infer struct{foo: 3}; return 42; }\n"
    ),
    "explicit-struct": (
        "fnc main(): int { const x = struct{foo: int}{foo: 3}; return 42; }\n"
    ),
}


def run(compiler: pathlib.Path, source: pathlib.Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(compiler), str(source)],
        text=True, capture_output=True, check=False, timeout=30,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    compiler = parser.parse_args().compiler.resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-shape-") as tmp:
        directory = pathlib.Path(tmp)
        for name, text in [("ordinary", CONTROL), ("comments", CONTROL_COMMENTS)]:
            source = directory / (name + ".ores")
            source.write_text(text, encoding="utf-8")
            result = run(compiler, source)
            if result.returncode != 0 or "define" not in result.stdout:
                raise AssertionError(
                    f"{name}: positive control failed, rejection checks inconclusive"
                )
        for name, text in REJECTED.items():
            source = directory / (name + ".ores")
            source.write_text(text, encoding="utf-8")
            result = run(compiler, source)
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError(
                    f"{name}: unsupported shape produced success or partial LLVM IR"
                )
            if "native structural shape boundary unsupported" not in result.stderr:
                raise AssertionError(
                    f"{name}: incidental failure instead of compiler-owned admission"
                )
        print(f"native readonly shape fail-closed: {len(REJECTED)} cases + controls")


if __name__ == "__main__":
    main()
