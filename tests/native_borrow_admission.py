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
        "borrowed reference may only be passed to a borrowed parameter",
    ),
    # Constructor by-value field initialization is an ownership transfer.
    "borrow-to-owned-constructor": (
        "pub define class Box as let int value=start;\n  pub constructor(int start){return;}\n  pub read()->int{return self.value;}\nend\npub fnc main(int v): int { let Box box = new Box(rt borrow v); return box.read(); }\n",
        "native scalar constructor argument requires pure integer expression",
    ),
    # The native generator frontend does not implement lexical loans yet.
    "generator-yield-loan-unsupported": (
        "generator fnc values(): int { let int x = 1; yield rt borrow x; return; }\n",
        "native generator v1 requires iterator-frame, ownership and LLVM continuation lowering",
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
    # A broken parser or unconditional rejection must never satisfy the
    # negative suite. Require three accepted source shapes to emit LLVM IR.
    valid_cases = {
        "valid-shared-borrow": (
            "fnc read(borrow int x): int { return x; }\n"
            "pub fnc main(int v): int { return read(rt borrow v); }\n"
        ),
        "valid-owned-scalar-call": (
            "fnc consume(int x): int { return x; }\n"
            "pub fnc main(int v): int { return consume(v); }\n"
        ),
        "valid-owned-constructor": (
            "pub define class Box as let int value=start;\n"
            "  pub constructor(int start){return;}\n"
            "  pub read()->int{return self.value;}\n"
            "end\n"
            "pub fnc main(int v): int { let Box box = new Box(v); return box.read(); }\n"
        ),
    }
    with tempfile.TemporaryDirectory() as folder:
        for title, source in valid_cases.items():
            path = pathlib.Path(folder) / (title + ".ores")
            path.write_text(source, encoding="utf-8")
            result = subprocess.run([sys.argv[1], str(path)], capture_output=True,
                                    text=True, check=False, timeout=20)
            if result.returncode != 0 or "define" not in result.stdout:
                raise AssertionError(
                    f"{title}: expected LLVM IR, got exit {result.returncode}: "
                    + result.stderr
                )
    print(f"native borrow admission: {len(CASES)} invalid programs rejected "
          f"without IR; {len(valid_cases)} valid controls emitted IR")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
