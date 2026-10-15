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
    compiler = Path(sys.argv[1]).resolve(strict=True)
    if not compiler.is_file():
        raise ValueError("compiler must be a regular executable file")
    with tempfile.TemporaryDirectory(prefix="ores-sealed-native-") as temp:
        for name, source in CASES.items():
            # Run in a clean, per-case working directory. A failing compiler
            # must not leave .ll/.bc/object artifacts that stdout checks miss.
            case_dir = Path(temp) / name
            case_dir.mkdir()
            path = case_dir / "input.ores"
            path.write_text(source.strip() + "\n", encoding="utf-8")
            result = subprocess.run(
                [str(compiler), str(path)], cwd=case_dir,
                capture_output=True, text=True, check=False, timeout=30,
            )
            unexpected_files = sorted(str(item.relative_to(case_dir))
                for item in case_dir.rglob("*") if item.is_file() and item != path)
            if (result.returncode != 1 or result.stdout.strip() or unexpected_files
                    or "native sealed/final/permits hierarchy unsupported" not in result.stderr):
                raise AssertionError(
                    f"{name}: compiler must reject unimplemented sealed "
                    f"hierarchy semantics without emitting IR or files: rc={result.returncode}; "
                    f"stdout={result.stdout!r}; stderr={result.stderr!r}; "
                    f"unexpected_files={unexpected_files!r}"
                )
    print(f"PASS: {len(CASES)} sealed/final/permits source forms fail closed")

if __name__ == "__main__":
    try:
        main()
    except (ValueError, AssertionError, OSError, subprocess.TimeoutExpired) as error:
        print(f"native sealed hierarchy parity gate failed: {error}", file=sys.stderr)
        raise SystemExit(1)
