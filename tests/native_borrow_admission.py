#!/usr/bin/env python3
"""Regression: LLVM native borrow admission must reject invalid ownership."""
import pathlib
import subprocess
import sys
import tempfile

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
        "fnc edit(borrow mut int x): int { return x; }\nfnc inner(borrow int x): int { return edit(rt borrow mut x); }\npub fnc main(int v): int { return inner(rt borrow v); }\n",
        "cannot mutably reborrow shared parameter",
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
    print("native borrow admission: four invalid programs rejected without LLVM IR")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
