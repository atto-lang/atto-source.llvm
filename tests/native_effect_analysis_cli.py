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

    # A hidden exceptional operation inside a direct-call argument must
    # not be laundered into a successful closed-effect receipt.
    argument_effect = run(
        compiler,
        "fnc identity(int value): int { return value; }\n"
        "pub fnc main(): int { return identity(1 + 2); }\n",
        flags=("--analyze",),
    )
    assert argument_effect.returncode == 2, (
        argument_effect.returncode, argument_effect.stdout, argument_effect.stderr
    )
    assert "status=inconclusive" in argument_effect.stdout
    assert "main/0\tstatus=inconclusive" in argument_effect.stdout, argument_effect.stdout
    # The unrelated `identity` definition may still have a known narrow-scope row.

    # Transitive unknown observations must contaminate the callers too.
    transitive_unknown = run(
        compiler,
        "fnc arithmetic(): int { return 1 + 2; }\n"
        "pub fnc main(): int { return arithmetic(); }\n",
        flags=("--analyze",),
    )
    assert transitive_unknown.returncode == 2, (
        transitive_unknown.returncode, transitive_unknown.stdout,
        transitive_unknown.stderr
    )
    assert "main/0\tstatus=inconclusive" in transitive_unknown.stdout
    assert "arithmetic/0\tstatus=inconclusive" in transitive_unknown.stdout

    # Name/arity call identity does not prove named-argument binding.
    # The parser may reject this spelling or admit it as an unknown effect;
    # neither is permission to return a known clean-effects receipt.
    named = run(
        compiler,
        "fnc identity(int value): int { return value; }\n"
        "pub fnc main(): int { return identity(value=42); }\n",
        flags=("--analyze",),
    )
    assert named.returncode != 0, (named.returncode, named.stdout, named.stderr)
    assert "main/0\tstatus=known" not in named.stdout

    # A long acyclic graph must not overflow recursive closure traversal or
    # produce a misleading successful purity receipt (depth budget: 64).
    functions = [
        f"fnc step_{i}(): int {{ return step_{i + 1}(); }}"
        for i in range(70)
    ]
    functions.append("fnc step_70(): int { return 42; }")
    functions.append("pub fnc main(): int { return step_0(); }")
    deep_graph = run(
        compiler, "\n".join(functions) + "\n", flags=("--analyze",)
    )
    assert deep_graph.returncode != 0, (
        deep_graph.returncode, deep_graph.stdout, deep_graph.stderr
    )
    # Independently resolved shallow leaves may be known; the deep root may not.
    assert "main/0\tstatus=inconclusive" in deep_graph.stdout or not deep_graph.stdout

    # Duplicate and incompatible selectors are invocation errors (exit 2),
    # never an advisory PASS, and must not emit even partial analysis records.
    for options in [
        ("--analyze", "--analyze"),
        ("--analyze", "--rt-check"),
        ("--analyze", "--constraint-descriptors"),
        ("--analyze", "--target=wasm32"),
    ]:
        rejected = run(compiler, pure, flags=options)
        assert rejected.returncode == 2, (
            options, rejected.returncode, rejected.stdout, rejected.stderr
        )
        assert not rejected.stdout, (options, rejected.stdout)

    # A self-recursive real source must not be reported as effect-free.
    # The native compiler may reject recursion during admission (exit 1)
    # or admit it for an inconclusive analysis (exit 2); neither is a PASS.
    recursive = run(
        compiler,
        "fnc recurse(): int { return recurse(); }\n"
        "pub fnc main(): int { return recurse(); }\n",
        flags=("--analyze",),
    )
    assert recursive.returncode != 0, (
        recursive.returncode, recursive.stdout, recursive.stderr
    )
    assert "main/0\tstatus=known" not in recursive.stdout

    # An unresolved target is not a trusted typed-effect summary.
    unresolved = run(
        compiler,
        "pub fnc main(): int { return undefined_helper(); }\n",
        flags=("--analyze",),
    )
    assert unresolved.returncode != 0, (
        unresolved.returncode, unresolved.stdout, unresolved.stderr
    )
    assert "main/0\tstatus=known" not in unresolved.stdout

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
