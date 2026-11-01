#!/usr/bin/env python3
"""Real LLVM frontend-admission tests, not guest critical runtime claims."""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

# Only actual compiler-owned expected diagnostics count. A crash, unrelated
# syntax failure or stdout/IR emission must fail the test.
NEGATIVES = [
    ("critical-suspends", "critical suspends fnc main(): int { return 42; }",
     "E_CALLABLE_MODIFIERS"),
    ("suspends-critical", "suspends critical fnc main(): int { return 42; }",
     "E_CALLABLE_MODIFIERS"),
    ("pub-critical-suspends", "pub critical suspends fnc main(): int { return 42; }",
     "E_CALLABLE_MODIFIERS"),
    ("critical-pub-body", "critical fnc main(): int { return 42; }",
     "E_CRITICAL_UNSUPPORTED"),
    ("pub-critical", "pub critical fnc main(): int { return 42; }",
     "E_CRITICAL_UNSUPPORTED"),
    ("suspends-pub", "suspends fnc main(): int { return 42; }",
     "native suspends declaration requires suspension-aware borrow checking"),
]

def run(compiler, source):
    return subprocess.run(
        [str(compiler), str(source)],
        input=b"", stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=15, check=False,
    )

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=Path, required=True)
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    if not compiler.is_file():
        raise RuntimeError("compiler executable unavailable")
    with tempfile.TemporaryDirectory(prefix="oreslang-critical-effects-") as td:
        path = Path(td) / "candidate.ores"
        for name, source, diagnostic in NEGATIVES:
            path.write_text(source + "\n", encoding="utf-8")
            observed = run(compiler, path)
            if (observed.returncode != 1 or observed.stdout or
                    diagnostic.encode("utf-8") not in observed.stderr or
                    b"panicked" in observed.stderr):
                raise RuntimeError(
                    f"{name}: expected exit 1, no IR, exact diagnostic {diagnostic}; "
                    f"got {(observed.returncode, observed.stdout[:80], observed.stderr[:300])!r}"
                )
        # Positive: do not accidentally prohibit lexical mention in comments.
        path.write_text("pub fnc main(): int { return 42; } // critical suspends\n",
                        encoding="utf-8")
        positive = run(compiler, path)
        if positive.returncode != 0 or not positive.stdout or positive.stderr:
            raise RuntimeError("scalar source unexpectedly rejected or no LLVM IR emitted")
    print(f"native compiler modifier boundary: {len(NEGATIVES)} negative + 1 positive")
    return 0

if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as exc:
        print("critical/suspends native LLVM admission DENIED: " + str(exc), file=sys.stderr)
        sys.exit(1)
