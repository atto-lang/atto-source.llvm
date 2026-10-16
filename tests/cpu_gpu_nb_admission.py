#!/usr/bin/env python3
"""Real native compiler gate for not-yet-lowered CPU/GPU/nb source regions.

Requires an actual oreslang-llvmc executable, not a lexer mock.
No positive GPU execution is claimed; must reject before publishing LLVM IR.
"""
import argparse
import pathlib
import subprocess
import tempfile

UNSUPPORTED = (
    "fnc main(): int { const foo = gpu {}; return 1; }",
    "fnc main(): int { const foo = cpu {}; return 1; }",
    "fnc main(): int { const foo = nb gpu {}; return 1; }",
    "fnc main(): int { const foo = nb cpu {}; return 1; }",
    "fnc main(): int { gpu { cpu { gpu {} } }; return 1; }",
    "fnc main(): int { nb gpu { nb cpu {} }; return 1; }",
    "fnc main(): int { return gpu; }",
    "fnc main(): int { return cpu; }",
)

def compile_source(compiler, text, path):
    path.write_text(text, encoding="utf-8")
    return subprocess.run([compiler, str(path)], capture_output=True,
                          text=True, check=False, timeout=15)

def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("--compiler", required=True)
    compiler = cli.parse_args().compiler
    with tempfile.TemporaryDirectory(prefix="ores-exec-domain-") as root:
        source = pathlib.Path(root) / "main.ores"
        for case in UNSUPPORTED:
            result = compile_source(compiler, case, source)
            if result.returncode != 1 or result.stdout or not result.stderr.strip():
                raise AssertionError(
                    f"unsupported CPU/GPU/nb source did not fail closed: {case!r}; "
                    f"return={result.returncode}, stdout={result.stdout[:200]!r}, "
                    f"stderr={result.stderr[:200]!r}")
        control = compile_source(
            compiler,
            "fnc main(): int { return 42; } // nb gpu { cpu {} }\n",
            source)
        if control.returncode or not control.stdout.startswith("; ModuleID"):
            raise AssertionError(
                f"comment control failed: rc={control.returncode}, "
                f"stderr={control.stderr[:300]!r}")
    print("native CPU/GPU/nb fail-closed source admission: PASS")

if __name__ == "__main__":
    main()
