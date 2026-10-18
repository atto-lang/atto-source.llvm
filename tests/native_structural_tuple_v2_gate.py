#!/usr/bin/env python3
"""Exact compiler gate for unsupported LLVM structural positional tuple v2."""
from pathlib import Path
import subprocess
import sys
import tempfile

CASES = {
    "explicit": "fnc take(tuple (int, int, string?) value): int { return 1; }\n",
    "shorthand": "fnc take((int, int, string?) value): int { return 1; }\n",
    "structural": "fnc take(structural (int, int, string?) value): int { return 1; }\n",
}
TUPLE_DIAG = "native positional tuple parameter requires verified readonly-slot and optional-tail lowering"
STRUCTURAL_DIAG = "native structural positional tuple requires verified nonescaping readonly-view lowering"

def snapshot(root: Path):
    result = {}
    for item in root.iterdir():
        if item.is_symlink() or not item.is_file():
            raise RuntimeError("unexpected compiler artifact type: " + item.name)
        result[item.name] = (item.stat().st_mode, item.read_bytes())
    return result

def run(compiler: Path, source: Path, cwd: Path):
    return subprocess.run([str(compiler), str(source)], cwd=cwd, text=True,
                          capture_output=True, timeout=15, check=False)

def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: native_structural_tuple_v2_gate.py <oreslang-llvmc>")
    compiler = Path(sys.argv[1]).resolve(strict=True)
    with tempfile.TemporaryDirectory(prefix="ores-tuple-v2-") as td:
        root = Path(td)
        good = root / "control.ores"
        good.write_text("pub fnc main(): int { return 42; }\n", encoding="utf-8")
        result = run(compiler, good, root)
        if result.returncode != 0 or "define i64 @__oreslang_guest_main(" not in result.stdout:
            raise RuntimeError("positive scalar compiler control failed: " + result.stderr[:500])
        if set(snapshot(root)) != {good.name}:
            raise RuntimeError("positive control unexpectedly published an artifact")
        good.unlink()

        for name, source in CASES.items():
            path = root / f"{name}.ores"
            path.write_text(source + "fnc main(): int { return 1; }\n", encoding="utf-8")
            before = snapshot(root)
            result = run(compiler, path, root)
            after = snapshot(root)
            if before != after:
                raise RuntimeError(f"{name}: rejected input or directory was modified")
            if result.returncode <= 0:
                raise RuntimeError(f"{name}: unsupported tuple source was accepted")
            required = STRUCTURAL_DIAG if name == "structural" else TUPLE_DIAG
            if required not in result.stderr:
                raise RuntimeError(f"{name}: missing semantic tuple-v2 diagnostic: {result.stderr[:500]}")
            if result.stdout.strip():
                raise RuntimeError(f"{name}: rejected source leaked compiler output")
    print("LLVM structural tuple v2 correctly fails closed (not runtime parity)")

if __name__ == "__main__":
    main()
