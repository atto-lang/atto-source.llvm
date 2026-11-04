#!/usr/bin/env python3
"""Real native compiler --analyze smoke: source admission, no IR, no guest execution.

This executes the target oreslang-llvmc, not a stub. It does not certify
transitive exceptional effects for the full AttoLang language.
"""
import pathlib
import subprocess
import sys
import tempfile


def run(compiler, source, suffix=".atto", flags=()):
    with tempfile.TemporaryDirectory(prefix="atto-native-analyze-") as tmp:
        root = pathlib.Path(tmp)
        path = root / ("test" + suffix)
        path.write_text(source, encoding="utf-8")
        before = path.read_bytes()
        proc = subprocess.run(
            [compiler, *flags, str(path)],
            cwd=root,
            stdin=subprocess.DEVNULL,
            capture_output=True,
            text=True,
            timeout=20,
            check=False,
        )
        assert path.read_bytes() == before, "analysis modified source"
        assert sorted(p.name for p in root.iterdir()) == [path.name], (
            "no-codegen analysis created an artifact"
        )
        assert "define " not in proc.stdout and "target triple" not in proc.stdout, (
            "analysis leaked LLVM IR"
        )
        return proc


def main(compiler):
    pure = "pub fnc main(): int { return 42; }\n"
    for extension in (".atto", ".ores"):
        result = run(compiler, pure, extension, ("--analyze",))
        assert result.returncode == 0, (result.returncode, result.stderr)
        assert "ORES-ANALYZE-V0" in result.stdout, result.stdout
        assert "status=known" in result.stdout, result.stdout
        assert "known_effects=none" in result.stdout, result.stdout

    # A real, admitted native source with a closed direct call chain:
    chain = (
        "fnc helper(): int { return 7; }\n"
        "pub fnc main(): int { return helper(); }\n"
    )
    result = run(compiler, chain, flags=("--analyze",))
    assert result.returncode == 0, (result.returncode, result.stderr)
    assert "main/0" in result.stdout
    assert "helper/0" in result.stdout

    # Checked arithmetic is not proved effect-free by the current native
    # direct-facts collector. It must not become a positive effect certificate.
    arithmetic = run(compiler,
                     "pub fnc main(): int { return 1 + 2; }\n",
                     flags=("--analyze",))
    assert arithmetic.returncode == 2, (arithmetic.returncode, arithmetic.stderr)
    assert "status=inconclusive" in arithmetic.stdout, arithmetic.stdout

    # Parsed try/catch source has an exceptional edge. Its effect closure is
    # not yet supported by v0 even when ordinary LLVM guest lowering exists.
    caught = run(compiler, """pub fnc main(): int {
  try {
    throw 43;
  } catch (err) {
    return err;
  } finally {
  }
}
""", flags=("--analyze",))
    assert caught.returncode == 2, (caught.returncode, caught.stderr)
    assert "status=inconclusive" in caught.stdout, caught.stdout

    # Malformed source cannot be reported as admitted or effect-free.
    malformed = run(compiler, "pub fnc main(): int { return ; }\n",
                    flags=("--analyze",))
    assert malformed.returncode != 0
    assert "status=known" not in malformed.stdout

    # Unsupported CLI combination must fail before reading/compiling.
    incompatible = run(compiler, pure, flags=("--analyze", "--tree-shake"))
    assert incompatible.returncode == 2
    assert not incompatible.stdout

    # No source is never a passing clean analysis.
    empty = run(compiler, "", flags=("--analyze",))
    assert empty.returncode != 0
    assert "status=known" not in empty.stdout


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: native_effect_analysis_cli.py /path/to/oreslang-llvmc")
    main(sys.argv[1])
