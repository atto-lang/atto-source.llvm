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
        hashes = {path.name: digest(path) for path in [atto, ores, bad]}
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

        for args in [("--analyze", "--rt-check", str(atto)),
                     ("--analyze", "--analyze", str(atto))]:
            result = run(*args)
            assert result.returncode == 2, (args, result.returncode, result.stderr)
            assert result.stdout == "", "rejected flags must not emit LLVM IR"

        assert sorted(path.name for path in root.iterdir()) == original_names
        for path in [atto, ores, bad]:
            assert digest(path) == hashes[path.name], f"compiler modified {path.name}"
    print("LLVM --analyze boundary: native source validated, effects remain inconclusive")
    return 0


if __name__ == "__main__":
    sys.exit(main())
