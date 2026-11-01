#!/usr/bin/env python3
"""Native LLVM frontend admission; actual compiler invocation, not source-text scanning."""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

CONTROL = "pub fnc main(): int { return 42; }\n"
CASES = {
    "critical_callable": ("pub critical fnc main(): int { return 42; }\n", "E_CRITICAL_UNLOWERED"),
    "critical_then_suspends": ("pub critical suspends fnc main(): int { return 42; }\n", "E_CRITICAL_SUSPENDS_DECL"),
    "suspends_then_critical": ("pub suspends critical fnc main(): int { return 42; }\n", "E_CRITICAL_SUSPENDS_DECL"),
}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()
    compiler = shutil.which(args.compiler)
    if compiler is None:
        raise RuntimeError("real LLVM compiler required; missing " + args.compiler)
    compiler = str(Path(compiler).resolve())
    with tempfile.TemporaryDirectory(prefix="critical-native-admission-") as work:
        base = Path(work)
        for name, (source, diag) in {"control": (CONTROL, None), **CASES}.items():
            path = base / (name + ".ores")
            path.write_text(source, encoding="utf-8")
            run = subprocess.run([compiler, str(path)], cwd=base,
                                 capture_output=True, text=True, check=False, timeout=20)
            produced = [p.name for p in base.iterdir()
                        if p.is_file() and p.suffix not in {".ores"}]
            if produced:
                raise AssertionError(f"{name}: produced unexpected artifacts {produced}")
            if diag is None:
                if run.returncode != 0 or "define i64" not in run.stdout:
                    raise AssertionError(f"positive control did not compile to native LLVM IR: {run.stderr[:400]}")
            elif run.returncode == 0 or run.stdout.strip() or diag not in run.stderr:
                raise AssertionError(f"{name}: expected {diag}, status={run.returncode}, "
                                     f"stdout={run.stdout[:200]!r}, stderr={run.stderr[:400]!r}")
    print("PASS: LLVM frontend critical+suspends declaration diagnostics from real compiler")
    return 0

if __name__ == "__main__":
    sys.exit(main())
