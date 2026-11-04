#!/usr/bin/env python3
"""Exercise the *real* LLVM compiler --analyze binary; no test compiler stubs."""
import argparse
import pathlib
import subprocess
import tempfile

def run(compiler, *args):
    return subprocess.run([str(compiler), *map(str, args)],
                          capture_output=True, text=True, timeout=30, check=False)

def verify():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    args = parser.parse_args()
    fixture = pathlib.Path(__file__).parent / "fixtures" / "native-caught-throw-payload.ores"
    with tempfile.TemporaryDirectory(prefix="atto-llvm-analyze-") as directory:
        root = pathlib.Path(directory)
        valid = root / "valid.atto"
        valid.write_text("pub fnc main(): int { return 42; }\n")
        original = valid.read_bytes()
        clean = run(args.compiler, "--analyze", valid)
        assert clean.returncode == 0, (clean.returncode, clean.stdout, clean.stderr)
        assert "ANALYZE ADVISORY backend=llvm" in clean.stdout, clean
        assert "warnings=0" in clean.stdout, clean.stdout
        assert "define i64" not in clean.stdout, "LLVM IR leaked on analysis"
        assert valid.read_bytes() == original, "analysis modified its input"

        thrown = root / "caught_throw.atto"
        thrown.write_text(fixture.read_text())
        original = thrown.read_bytes()
        effect = run(args.compiler, "--analyze", thrown)
        assert effect.returncode == 0, (effect.returncode, effect.stdout, effect.stderr)
        assert "EFFECT NOTE main: local caught throw" in effect.stdout, effect.stdout
        assert "warnings=0" in effect.stdout, effect.stdout
        assert "handled_throws=1" in effect.stdout, effect.stdout
        assert "escaping_transitive_bits=0" in effect.stdout, effect.stdout
        assert thrown.read_bytes() == original, "effect analysis modified its input"
        assert "define i64" not in effect.stdout, "LLVM IR leaked on analysis"

        # Transitive exceptional effects must reach callers through the AST.
        transitive = root / "transitive.atto"
        transitive.write_text(
            "fnc caught(): int { try { throw 7; } catch (err) "
            "{ return err; } finally {} }\n"
            "pub fnc main(): int { return caught(); }\n"
        )
        transitive_run = run(args.compiler, "--analyze", transitive)
        assert transitive_run.returncode == 0, (
            transitive_run.returncode, transitive_run.stdout, transitive_run.stderr)
        assert "EFFECT NOTE caught: local caught throw" in transitive_run.stdout
        assert "EFFECT WARNING main:" not in transitive_run.stdout
        assert "warnings=0" in transitive_run.stdout
        assert "handled_throws=1" in transitive_run.stdout

        recursive = root / "recursive.atto"
        recursive.write_text(
            "fnc recurse(int n): int { return recurse(n); }\n"
            "pub fnc main(): int { return recurse(1); }\n"
        )
        recursive_result = run(args.compiler, "--analyze", recursive)
        assert recursive_result.returncode != 0, (
            recursive_result.returncode, recursive_result.stdout)
        assert "ANALYZE ADVISORY" not in recursive_result.stdout

        malformed = root / "malformed.atto"
        malformed.write_text("pub fnc main(): int { return ; }\n")
        failure = run(args.compiler, "--analyze", malformed)
        assert failure.returncode != 0, (failure.returncode, failure.stdout)
        assert "ANALYZE ADVISORY" not in failure.stdout, failure.stdout

        duplicate = run(args.compiler, "--analyze", "--analyze", valid)
        assert duplicate.returncode == 2, (duplicate.returncode, duplicate.stderr)
        wasm = run(args.compiler, "--analyze", "--target=wasm32", valid)
        assert wasm.returncode == 2, (wasm.returncode, wasm.stderr)
        assert "ANALYZE ADVISORY" not in wasm.stdout

        conflict = run(args.compiler, "--analyze", "--rt-check", valid)
        assert conflict.returncode == 2, (conflict.returncode, conflict.stderr)
        assert "ANALYZE ADVISORY" not in conflict.stdout, conflict.stdout
        print("native --analyze real-compiler admission, warning and no-mutation: PASS")

if __name__ == "__main__":
    verify()
