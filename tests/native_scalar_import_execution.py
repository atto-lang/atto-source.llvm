#!/usr/bin/env python3
"""End-to-end native scalar imports: actual compiler IR, Clang, and process exit."""
import argparse
from pathlib import Path
import subprocess
import tempfile

def run(*args, input=None):
    return subprocess.run(args, input=input, text=True, capture_output=True, timeout=45)

def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")

def expect_success(compiler, clang, source, exit_code=42):
    produced = run(compiler, "--experimental-scalar-imports", str(source))
    if produced.returncode != 0:
        raise AssertionError("scalar import compilation failed: " + produced.stderr)
    if "define" not in produced.stdout:
        raise AssertionError("compiler did not emit LLVM definitions")
    binary = source.parent / "compiled-scalar-import"
    linked = run(clang, "-x", "ir", "-o", str(binary), "-", input=produced.stdout)
    if linked.returncode != 0:
        raise AssertionError("Clang rejected emitted IR: " + linked.stderr)
    executed = run(str(binary))
    if executed.returncode != exit_code:
        raise AssertionError(f"imported program exited {executed.returncode}, wanted {exit_code}: " + executed.stderr)

def expect_failure(compiler, source, diagnostic):
    result = run(compiler, "--experimental-scalar-imports", str(source))
    if result.returncode == 0 or diagnostic not in result.stderr:
        raise AssertionError(f"expected rejection containing {diagnostic!r}: {result.returncode} {result.stderr}")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-native-scalar-import-") as directory:
        root = Path(directory)
        write(root / "helper.ores", "fnc add(int a, int b): int { return a + b; }\n")
        write(root / "main.ores", 'import "./helper";\npub fnc main(): int { return add(40, 2); }\n')
        expect_success(args.compiler, args.clang, root / "main.ores")
        write(root / "extra.ores", 'fnc extra(int value): int { return value + 1; }\n')
        write(root / "reorder.ores", 'import "./extra";\nimport "./helper";\npub fnc main(): int { return extra(add(39, 2)); }\n')
        expect_success(args.compiler, args.clang, root / "reorder.ores")
        write(root / "cycle_a.ores", 'import "./cycle_b";\npub fnc main(): int { return 42; }\n')
        write(root / "cycle_b.ores", 'import "./cycle_a";\nfnc unused(): int { return 1; }\n')
        expect_failure(args.compiler, root / "cycle_a.ores", "cyclic native scalar imports")
        write(root / "bad.ores", 'import "./missing";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "bad.ores", "native scalar import source not found")
        write(root / "absolute.ores", 'import "/tmp/outside";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "absolute.ores", "absolute native scalar imports")
        write(root / "foreign.java", "not a native module\n")
        write(root / "foreign_import.ores", 'import "./foreign";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "foreign_import.ores", "native imports support only .ores sources")
        write(root / "package" / ".ores.pkg.toml", 'schema_version = "1"\n[source]\nroots = ["src"]\n')
        write(root / "package" / "src" / "main.ores", 'import "@/src/util";\npub fnc main(): int { return helper(40); }\n')
        write(root / "package" / "src" / "util.ores", 'fnc helper(int a): int {return a + 2;}\n')
        expect_success(args.compiler, args.clang, root / "package" / "src" / "main.ores")
        write(root / "package" / "src" / "escape.ores", 'import "../../helper";\npub fnc main(): int {return add(40, 2);}\n')
        expect_failure(args.compiler, root / "package" / "src" / "escape.ores", "relative import escaped declared package root")
    print("native scalar import execution gates passed")

if __name__ == "__main__":
    main()
