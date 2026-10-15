#!/usr/bin/env python3
"""Sealed hierarchy syntax is not native LLVM lowering: reject without any IR."""
from pathlib import Path
import subprocess
import sys
import tempfile

CASES = {
    "sealed-class-explicit": """
        define sealed class WebEvent permits Click, Scroll as end
        define final class Click from WebEvent as end
        define final class Scroll extends WebEvent as end
        pub fnc main(): int { return 0; }
    """,
    "sealed-class-inferred": """
        define sealed class WebEvent as end
        define final class Click extends WebEvent as end
        pub fnc main(): int { return 0; }
    """,
    "final-class": """
        define final class Click as end
        pub fnc main(): int { return 0; }
    """,
    "sealed-interface": """
        define sealed interface WebEvent permits Click as end
        define final class Click implements WebEvent as end
        pub fnc main(): int { return 0; }
    """,
    "permits-without-sealed": """
        define class WebEvent permits Click as end
        define class Click extends WebEvent as end
        pub fnc main(): int { return 0; }
    """,
    "from-parent-alias": """
        define class Parent as end
        define class Child from Parent as end
        pub fnc main(): int { return 0; }
    """,
}

def main() -> None:
    if len(sys.argv) != 2:
        raise ValueError("expected compiler executable path")
    compiler = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="ores-sealed-native-") as temp:
        for name, source in CASES.items():
            path = Path(temp) / f"{name}.ores"
            path.write_text(source.strip() + "\n", encoding="utf-8")
            result = subprocess.run(
                [compiler, str(path)],
                capture_output=True, text=True, check=False, timeout=30,
            )
            if result.returncode == 0 or result.stdout.strip():
                raise AssertionError(
                    f"{name}: compiler must reject unimplemented sealed "
                    f"hierarchy semantics before emitting IR: rc={result.returncode}; "
                    f"stdout={result.stdout!r}; stderr={result.stderr!r}"
                )
    print(f"PASS: {len(CASES)} sealed/final/permits source forms fail closed")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"native sealed hierarchy parity gate failed: {error}", file=sys.stderr)
        raise SystemExit(1)
