#!/usr/bin/env python3
"""Real native compiler gate for not-yet-lowered CPU/GPU/nb source regions.

Requires an actual oreslang-llvmc executable, not a lexer mock.
No positive GPU execution is claimed; must reject before publishing LLVM IR.
"""
import argparse
import pathlib
import re
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
                          text=True, check=False, timeout=15, cwd=path.parent)

def main():
    cli = argparse.ArgumentParser()
    cli.add_argument("--compiler", required=True)
    compiler = str(pathlib.Path(cli.parse_args().compiler).resolve(strict=True))
    with tempfile.TemporaryDirectory(prefix="ores-exec-domain-") as root:
        source = pathlib.Path(root) / "main.ores"
        for case in UNSUPPORTED:
            result = compile_source(compiler, case, source)
            if (result.returncode != 1 or result.stdout
                    or not re.search(r"\b(cpu|gpu|nb|nonblocking)\b|execution[- ]domain",
                                     result.stderr, re.IGNORECASE)):
                raise AssertionError(
                    f"unsupported CPU/GPU/nb source did not produce a domain-specific refusal: {case!r}; "
                    f"return={result.returncode}, stdout={result.stdout[:200]!r}, "
                    f"stderr={result.stderr[:200]!r}")
            if sorted(item.name for item in pathlib.Path(root).iterdir()) != ["main.ores"]:
                raise AssertionError("failed domain compilation emitted a sidecar artifact")
        control = compile_source(
            compiler,
            "fnc main(): int { return 42; } // nb gpu { cpu {} }\n",
            source)
        if control.returncode or not control.stdout.startswith("; ModuleID"):
            raise AssertionError(
                f"comment control failed: rc={control.returncode}, "
                f"stderr={control.stderr[:300]!r}")
        if sorted(item.name for item in pathlib.Path(root).iterdir()) != ["main.ores"]:
            raise AssertionError("positive scalar control published a sidecar artifact")
    print("native CPU/GPU/nb fail-closed source admission: PASS")

if __name__ == "__main__":
    main()
