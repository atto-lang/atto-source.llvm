#!/usr/bin/env python3
"""End-to-end exact-once conditional cleanup of experimental native owned i64."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(argv):
    return subprocess.run(argv, capture_output=True, text=True, timeout=90)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--include", required=True)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()

    examples = {
        "taken-final": "const x: owned int = 42; return rt take x;",
        "read-final": "const x: owned int = 42; return x;",
        "taken-early": "const x: owned int = 42; if (x eq 42) { return rt take x; } fi return x;",
        "untaken-early": "const x: owned int = 42; if (x eq 0) { return rt take x; } fi return x;",
        "stored-move": "const x: owned int = 42; const y: int = rt take x; return y;",
        "partial-double": "const a: owned int = 20; const b: owned int = 22; return rt take a + b;",
        "double-transfer": "const a: owned int = 20; const b: owned int = 22; return rt take a + rt take b;",
        "nontransfer-double": "const a: owned int = 20; const b: owned int = 22; return a + b;",
        "nested-guard": "const x: owned int = 42; const y: owned int = 1; if (y eq 1) { return rt take x; } fi return x;",
        "left-to-right": "const x: owned int = 40; const y: owned int = 2; return rt take x + rt take y;",
        "if-else-two-moves": (
            "const x: owned int = 42; "
            "if (1 eq 1) { consume(rt take x); } "
            "else { consume(rt take x); } fi "
            "return 42;"
        ),
    }
    negatives = {
        "use-after-move": (
            "const x: owned int = 42; const y: int = rt take x; return x;",
            "use of explicitly moved int",
        ),
        "double-take": (
            "const x: owned int = 42; return rt take x + rt take x;",
            "use of explicitly moved int",
        ),
        "divergent-join": (
            "const x: owned int = 42; "
            "if (1 eq 1) { consume(rt take x); } else { consume(0); } fi "
            "return 42;",
            "conditional CFG predecessor ownership mismatch",
        ),
        "defer-after-take": (
            "const x: owned int = 42; defer inspect(rt borrow x); return rt take x;",
            "borrow of explicitly moved int",
        ),
    }
    prelude = (
        "fnc consume(int value): void { return; }\n"
        "fnc inspect(borrow int value): void { return; }\n"
    )
    with tempfile.TemporaryDirectory(prefix="ores-owned-region-flags-") as tmp:
        root = Path(tmp)
        for label, program in examples.items():
            fixture = root / (label + ".ores")
            fixture.write_text(prelude + "pub fnc main(): int { " + program + " }\n")
            compiled = run([args.compiler, "--experimental-owned-int", str(fixture)])
            assert compiled.returncode == 0 and compiled.stdout.startswith("; ModuleID"), (
                label, compiled.stderr, compiled.stdout)
            assert ".drop.if.live" in compiled.stdout and "br i1" in compiled.stdout, (
                label, "runtime conditional drop flag missing")
            ll = root / (label + ".ll")
            ll.write_text(compiled.stdout)
            guest = root / (label + ".bin")
            command = [args.clang, "-O1", "-g", "-fno-omit-frame-pointer"]
            if args.sanitize:
                command.extend(["-fsanitize=address,undefined"])
            command.extend([
                "-x", "ir", str(ll),
                "-x", "c", args.runtime, "-I", args.include, "-std=c11",
                "-o", str(guest),
            ])
            link = run(command)
            assert link.returncode == 0, (label, link.stderr)
            executed = run([str(guest)])
            assert executed.returncode == 42, (label, executed.returncode, executed.stderr)
        for label, (program, message) in negatives.items():
            fixture = root / (label + ".ores")
            fixture.write_text(prelude + "pub fnc main(): int { " + program + " }\n")
            compiled = run([args.compiler, "--experimental-owned-int", str(fixture)])
            assert compiled.returncode != 0 and message in compiled.stderr, (
                label, message, compiled.stderr)
            assert not compiled.stdout.strip(), (label, "emitted IR for unsafe move")
        fixture = root / "wasm32-region.ores"
        fixture.write_text("pub fnc main(): int { const x: owned int = 42; return rt take x; }\n")
        compiled = run([args.compiler, "--experimental-owned-int", "--target=wasm32", str(fixture)])
        assert compiled.returncode != 0 and "wasm32 native heap-owned regions are not implemented" in compiled.stderr, compiled.stderr
        assert not compiled.stdout.strip(), "Wasm32 native-heap ABI admitted"
    print(f"PASS {len(examples)} source-to-native ownership flags, "
          f"{len(negatives)} no-IR ownership violations, Wasm fail-closed, "
          f"sanitizers={args.sanitize}")


if __name__ == "__main__":
    main()
