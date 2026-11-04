#!/usr/bin/env python3
"""Executable CLI boundary: LLVM may validate native source, not certify effects.

This test intentionally demands INCONCLUSIVE for valid input until compiler-owned
typed transitive throw/raise/panic inference is implemented and guest-verified.
"""
from __future__ import annotations

import hashlib
import pathlib
import subprocess
import sys
import tempfile


def digest(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    if not __debug__:
        raise RuntimeError("this test must not run with optimized-out assertions")
    if len(sys.argv) != 2:
        raise AssertionError("usage: native_effect_analyze_boundary.py <compiler>")
    compiler = pathlib.Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="llvm-analyze-boundary-") as tmp:
        root = pathlib.Path(tmp)
        valid = "pub routine main(): int {\n    return 0;\n}\n"
        atto = root / "valid.atto"
        ores = root / "legacy.ores"
        bad = root / "invalid.atto"
        atto.write_text(valid, encoding="utf-8")
        ores.write_text(valid, encoding="utf-8")
        bad.write_text("pub routine main(): int {\n    return ;\n}\n", encoding="utf-8")
        # Shared RFC #35 effect spellings are not admitted by the native
        # compiler's typed exceptional-effect pipeline yet. They may be
        # rejected (1) or remain inconclusive (2), but may never claim PASS.
        annotated = []
        for effect in ("throws", "raises", "panics"):
            source = root / f"annotated_{effect}.atto"
            source.write_text(
                f"pub routine main(): int {effect}(Error) {{\n"
                "    return 0;\n}\n",
                encoding="utf-8",
            )
            annotated.append(source)
        # Exact source-level observation fixtures. One is an already
        # admitted native caught-throw guest, not an invented host policy.
        caught = root / "caught_throw.atto"
        caught.write_bytes(
            (pathlib.Path(__file__).resolve().parent / "fixtures" /
             "native-caught-throw-payload.ores").read_bytes()
        )
        possible_panic = root / "potential_panic.atto"
        possible_panic.write_text(
            "fnc compute(int divisor): int {\n"
            "  return 11 % divisor;\n"
            "}\n"
            "pub fnc main(): int {\n"
            "  return compute(2);\n"
            "}\n",
            encoding="utf-8",
        )
        files = [atto, ores, bad, *annotated, caught, possible_panic]
        hashes = {path.name: digest(path) for path in files}
        original_names = sorted(path.name for path in root.iterdir())

        def run(*args: str) -> subprocess.CompletedProcess[str]:
            return subprocess.run(
                [str(compiler), *args],
                cwd=root,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=40,
                check=False,
            )

        for source in [atto, ores]:
            result = run("--analyze", str(source))
            assert result.returncode == 2, (source.name, result.returncode, result.stderr)
            assert "exception-effect analysis unsupported" in result.stderr, result.stderr
            assert "inconclusive" in result.stderr, result.stderr
            assert result.stdout == "", "analysis must never emit LLVM IR"
        invalid = run("--analyze", str(bad))
        assert invalid.returncode == 1, (invalid.returncode, invalid.stderr)
        assert invalid.stdout == "", "invalid input must not emit LLVM IR"

        for source, expected in ((caught, "throw_observed=1"),
                                 (possible_panic, "panic_observed=1")):
            result = run("--analyze", str(source))
            assert result.returncode == 2, (
                source.name, result.returncode, result.stderr
            )
            assert "compiler AST effect observations" in result.stderr
            assert expected in result.stderr, result.stderr
            assert "effects inconclusive" in result.stderr, result.stderr
            assert result.stdout == "", "analysis observation leaked LLVM IR"

        for source in annotated:
            result = run("--analyze", str(source))
            assert result.returncode in (1, 2), (
                source.name, result.returncode, result.stderr
            )
            assert result.stdout == "", "unadmitted effect grammar emitted LLVM IR"

        # Ensure unsupported modes cannot quietly change meaning of --analyze.
        incompatible = [
            "--rt-check", "--tree-shake", "--constraint-descriptors",
            "--target=host", "--profile=native-hard-rt",
            "--rt-report=analysis.txt", "--library-profile=std",
            "--allocator-provider=bounded-arena",
        ]
        rejected = [("--analyze", "--analyze", str(atto))]
        for flag in incompatible:
            rejected.append(("--analyze", flag, str(atto)))
            rejected.append((flag, "--analyze", str(atto)))
        for args in rejected:
            result = run(*args)
            assert result.returncode == 2, (args, result.returncode, result.stderr)
            assert result.stdout == "", "rejected flags must not emit LLVM IR"

        assert sorted(path.name for path in root.iterdir()) == original_names
        for path in files:
            assert digest(path) == hashes[path.name], f"compiler modified {path.name}"
    print("LLVM --analyze boundary: native source validated, effects remain inconclusive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
