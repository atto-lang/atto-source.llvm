#!/usr/bin/env python3
"""Regression: LLVM native borrow admission must reject invalid ownership."""
import pathlib
import subprocess
import sys
import tempfile

# Native supported-source rejections must be ownership-specific. The generator
# control is deliberately a fail-closed unsupported-feature gate, not a proof
# of generator lifetime analysis.
CASES = {
    "implicit-borrow": (
        "fnc get(borrow int x): int { return x; }\npub fnc main(int v): int { return get(v); }\n",
        "borrowed argument requires explicit rt borrow",
    ),
    "wrong-borrow-mutability": (
        "fnc get(borrow mut int x): int { return x; }\npub fnc main(int v): int { return get(rt borrow v); }\n",
        "borrow mode does not match borrowed parameter",
    ),
    "temporary-borrow": (
        "fnc get(borrow int x): int { return x; }\npub fnc main(): int { return get(rt borrow (40 + 2)); }\n",
        "native borrow requires an addressable named int",
    ),
    "shared-to-mutable": (
        "fnc edit(borrow mut int x): int { return x; }\nfnc inner(borrow int x): int { return edit(rt mut borrow x); }\npub fnc main(int v): int { return inner(rt borrow v); }\n",
        "cannot mutably reborrow shared parameter",
    ),
    # A borrow passed to an owned i64 slot must never be lowered by-value.
    "borrow-to-owned-parameter": (
        "fnc consume(int x): int { return x; }\npub fnc main(int v): int { return consume(rt borrow v); }\n",
        "borrow",
    ),
    # Constructor by-value field initialization is an ownership transfer.
    "borrow-to-owned-constructor": (
        "define class Box as\n  pub val int value;\nend\npub fnc main(int v): int { val Box box = new Box(rt borrow v); return box.value; }\n",
        "borrow",
    ),
    # The native generator frontend does not implement lexical loans yet.
    "generator-yield-loan-unsupported": (
        "fnc gen* values(): Iterator<int> { let int x = 1; yield rt borrow x; return; }\n",
        "native actor/async declaration requires",
    ),
}

def main():
    if len(sys.argv) != 2:
        raise ValueError("usage: native_borrow_admission.py COMPILER")
    with tempfile.TemporaryDirectory() as folder:
        for title, (source, expected) in CASES.items():
            path = pathlib.Path(folder) / (title + ".ores")
            path.write_text(source, encoding="utf-8")
            result = subprocess.run([sys.argv[1], str(path)], capture_output=True,
                                    text=True, check=False, timeout=20)
            if result.returncode == 0 or result.stdout.strip() or expected not in result.stderr:
                raise AssertionError(f"{title}: must reject without emitted IR: {result.stderr}")
    # A passing negative matrix is meaningless if compiler failures are
    # unconditional. Require one supported shared borrow to emit actual IR.
    with tempfile.TemporaryDirectory() as folder:
        path = pathlib.Path(folder) / "valid-shared-borrow.ores"
        path.write_text(
            "fnc read(borrow int x): int { return x; }\n"
            "pub fnc main(int v): int { return read(rt borrow v); }\n",
            encoding="utf-8",
        )
        result = subprocess.run([sys.argv[1], str(path)], capture_output=True,
                                text=True, check=False, timeout=20)
        if result.returncode != 0 or "define" not in result.stdout:
            raise AssertionError(
                "valid shared borrowing must produce LLVM IR: "
                + result.stderr
            )
    print(f"native borrow admission: {len(CASES)} invalid programs rejected "
          "without IR; one valid shared borrow emitted IR")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
