#!/usr/bin/env python3
"""End-to-end native scalar imports: actual compiler IR, Clang, and process exit."""
import argparse
import os
import re
from pathlib import Path
import subprocess
import tempfile

def run(*args, input=None, env=None):
    # Prevent the developer/runner's ambient paths from changing test results.
    clean = dict(os.environ)
    clean.pop("ORES_PATHS", None)
    clean.pop("ORESLANG_PATH", None)
    if env:
        clean.update(env)
    return subprocess.run(args, input=input, env=clean, text=True,
                          capture_output=True, timeout=45)

def write(path, content):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")

def expect_success(compiler, clang, source, exit_code=42, required_calls=(), env=None):
    produced = run(compiler, "--experimental-scalar-imports", str(source), env=env)
    if produced.returncode != 0:
        raise AssertionError("scalar import compilation failed: " + produced.stderr)
    if "define" not in produced.stdout:
        raise AssertionError("compiler did not emit LLVM definitions")
    # A correct exit status alone cannot prove an imported call was lowered.
    # Require a direct call in emitted, linker-accepted LLVM IR.
    for symbol in required_calls:
        pattern = r"\bcall\s+i64\s+@" + re.escape(symbol) + r"\s*\("
        if not re.search(pattern, produced.stdout):
            raise AssertionError(f"missing imported call to {symbol}: " + produced.stdout[:4000])
    binary = source.parent / "compiled-scalar-import"
    linked = run(clang, "-x", "ir", "-o", str(binary), "-", input=produced.stdout)
    if linked.returncode != 0:
        raise AssertionError("Clang rejected emitted IR: " + linked.stderr)
    executed = run(str(binary))
    if executed.returncode != exit_code:
        raise AssertionError(f"imported program exited {executed.returncode}, wanted {exit_code}: " + executed.stderr)

def expect_failure(compiler, source, diagnostic, env=None):
    result = run(compiler, "--experimental-scalar-imports", str(source), env=env)
    if result.returncode == 0 or diagnostic not in result.stderr:
        raise AssertionError(f"expected rejection containing {diagnostic!r}: {result.returncode} {result.stderr}")

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", required=True)
    args = p.parse_args()
    with tempfile.TemporaryDirectory(prefix="ores-native-scalar-import-") as directory:
        root = Path(directory)
        write(root / "helper.ores", "pub fnc add(int a, int b): int { return a + b; }\n")
        write(root / "main.ores", 'import "./helper";\npub fnc main(): int { return add(40, 2); }\n')
        expect_success(args.compiler, args.clang, root / "main.ores", required_calls=("add",))
        write(root / "extra.ores", 'pub fnc extra(int value): int { return value + 1; }\n')
        write(root / "reorder.ores", 'import "./extra";\nimport "./helper";\npub fnc main(): int { return extra(add(39, 2)); }\n')
        expect_success(args.compiler, args.clang, root / "reorder.ores", required_calls=("add", "extra"))
        # A diamond graph must deduplicate the shared canonical .ores source.
        write(root / "diamond" / "core.ores", "pub fnc core(): int { return 20; }\n")
        write(root / "diamond" / "left.ores",
              'import "./core";\npub fnc left(): int { return core(); }\n')
        write(root / "diamond" / "right.ores",
              'import "./core";\npub fnc right(): int { return core() + 2; }\n')
        write(root / "diamond" / "main.ores",
              'import "./left";\nimport "./right";\npub fnc main(): int { return left() + right(); }\n')
        expect_success(args.compiler, args.clang, root / "diamond" / "main.ores",
                       required_calls=("left", "right", "core"))

        # Charge *unique* module count, not import directive count, and fail
        # before source concatenation or code generation on oversized graphs.
        wide_imports = []
        for i in range(64):
            write(root / "wide" / f"part_{i}.ores",
                  f"pub fnc part_{i}(): int {{ return {i}; }}\n")
            wide_imports.append(f'import "./part_{i}";')
        write(root / "wide" / "main.ores",
              "\n".join(wide_imports) + "\npub fnc main(): int { return 42; }\n")
        expect_failure(args.compiler, root / "wide" / "main.ores",
                       "native scalar import module count exceeded")

        # Distinguish the graph-depth limit from the total-node limit.
        for i in range(34):
            preamble = f'import "./depth_{i+1}";\n' if i < 33 else ""
            write(root / "deep" / f"depth_{i}.ores",
                  preamble + f"pub fnc depth_fn_{i}(): int {{ return 1; }}\n")
        write(root / "deep" / "main.ores",
              'import "./depth_0";\npub fnc main(): int { return 42; }\n')
        expect_failure(args.compiler, root / "deep" / "main.ores",
                       "native scalar import depth exceeded")
        # Import closure is a source-concatenation experiment. Private
        # imported declarations must never be silently exposed to the caller.
        write(root / "private.ores", "fnc secret(): int { return 42; }\n")
        write(root / "private_caller.ores",
              'import "./private";\npub fnc main(): int { return secret(); }\n')
        expect_failure(args.compiler, root / "private_caller.ores",
                       "native scalar imported function must be pub")
        write(root / "private_entry.ores",
              'import "./helper";\nfnc local(): int { return 2; }\npub fnc main(): int { return add(40, local()); }\n')
        expect_failure(args.compiler, root / "private_entry.ores",
                       "native scalar import closure requires pub on all functions")
        write(root / "foreign_main.ores", "pub fnc main(): int { return 42; }\n")
        write(root / "imported_entry.ores",
              'import "./foreign_main";\nfnc helper(): int { return 1; }\n')
        expect_failure(args.compiler, root / "imported_entry.ores",
                       "native scalar imported source cannot define main")
        write(root / "cycle_a.ores", 'import "./cycle_b";\npub fnc main(): int { return 42; }\n')
        write(root / "cycle_b.ores", 'import "./cycle_a";\nfnc unused(): int { return 1; }\n')
        expect_failure(args.compiler, root / "cycle_a.ores", "cyclic native scalar imports")
        # Target directives must be diagnosed before source-line stripping.
        write(root / "directive_entry.ores",
              'import "./helper"; // ores:target=unsupported\npub fnc main(): int {return add(40, 2);}\n')
        expect_failure(args.compiler, root / "directive_entry.ores",
                       "native target admission unsupported")
        write(root / "directive_dependency.ores",
              'import "./extra"; // ores:target=unsupported\npub fnc forwarded(): int {return extra(41);}\n')
        write(root / "directive_caller.ores",
              'import "./directive_dependency";\npub fnc main(): int {return forwarded();}\n')
        expect_failure(args.compiler, root / "directive_caller.ores",
                       "native target admission unsupported")
        write(root / "bad.ores", 'import "./missing";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "bad.ores", "native scalar import source not found")
        write(root / "absolute.ores", 'import "/tmp/outside";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "absolute.ores", "absolute native scalar imports")
        write(root / "foreign.java", "not a native module\n")
        write(root / "foreign_import.ores", 'import "./foreign";\npub fnc main(): int {return 0;}\n')
        expect_failure(args.compiler, root / "foreign_import.ores", "native imports support only .ores sources")
        write(root / "package" / ".ores.pkg.toml", 'schema_version = "1"\n[source]\nroots = ["src"]\n')
        write(root / "package" / "src" / "main.ores", 'import "+/src/util";\npub fnc main(): int { return helper(40); }\n')
        write(root / "package" / "src" / "util.ores", 'pub fnc helper(int a): int {return a + 2;}\n')
        expect_success(args.compiler, args.clang, root / "package" / "src" / "main.ores", required_calls=("helper",))
        # First Oreslang-shaped named import. Unlike concatenation-only
        # imports, the selector must bind to the public function in this exact
        # physical module and cannot silently expose unrelated definitions.
        write(root / "package" / "src" / "named.ores",
              'import fnc helper from "+/src/util";\npub fnc main(): int { return helper(40); }\n')
        expect_success(args.compiler, args.clang, root / "package" / "src" / "named.ores",
                       required_calls=("helper",))
        write(root / "package" / "src" / "named_wrong.ores",
              'import fnc missing from "+/src/util";\npub fnc main(): int { return 42; }\n')
        expect_failure(args.compiler, root / "package" / "src" / "named_wrong.ores",
                       "native named import")
        write(root / "package" / "src" / "extra.ores",
              'pub fnc helper(int n): int { return n + 2; }\n'
              'pub fnc unrequested(): int { return 0; }\n')
        write(root / "package" / "src" / "named_leak.ores",
              'import fnc helper from "+/src/extra";\npub fnc main(): int { return helper(40); }\n')
        expect_failure(args.compiler, root / "package" / "src" / "named_leak.ores",
                       "native named import source contains unrequested function")
        write(root / "package" / "src" / "bad_module.ores",
              'import module util from "+/src/util";\npub fnc main(): int { return 42; }\n')
        expect_failure(args.compiler, root / "package" / "src" / "bad_module.ores",
                       "native import requires a quoted path")
        # Conformance with Java's [project].root: +/ is not permanently
        # anchored to the directory containing .ores.pkg.toml.
        configured = root / "configured"
        write(configured / ".ores.pkg.toml",
              'schema_version = "1"\n[project]\nroot = "app"\n[source]\nroots = ["src"]\n')
        write(configured / "app" / "src" / "helper.ores",
              'pub fnc helper(int v): int { return v + 2; }\n')
        write(configured / "app" / "src" / "main.ores",
              'import fnc helper from "+/src/helper";\npub fnc main(): int { return helper(40); }\n')
        expect_success(args.compiler, args.clang,
                       configured / "app" / "src" / "main.ores", required_calls=("helper",))
        write(configured / "outside.ores", 'pub fnc outside(): int { return 42; }\n')
        write(configured / "app" / "src" / "outside_main.ores",
              'import fnc outside from "+/outside";\npub fnc main(): int { return outside(); }\n')
        expect_failure(args.compiler, configured / "app" / "src" / "outside_main.ores",
                       "native scalar import source not found")

        # The canonical root marker is +/; the retired @/ marker must never
        # gain authority through the experimental import concatenation path.
        write(root / "package" / "src" / "retired.ores",
              'import "@/src/util";\npub fnc main(): int { return helper(40); }\n')
        expect_failure(args.compiler, root / "package" / "src" / "retired.ores",
                       "invalid project-root import prefix")
        for marker in ("+/src/./util", "+/src//util", "+/src/../src/util",
                       "+/src\\util"):
            write(root / "package" / "src" / "noncanonical.ores",
                  f'import "{marker}";\npub fnc main(): int {{ return helper(40); }}\n')
            expect_failure(args.compiler, root / "package" / "src" / "noncanonical.ores",
                           "rooted import")

        # One installed zed-pkg alias, with physical package metadata checked.
        write(root / "package" / ".ores.pkg.toml",
              'schema_version = "1"\n[source]\nroots = ["src"]\n[dependencies]\nmath = { zed = "acme/math" }\n')
        write(root / "package" / "zed_modules" / "acme" / "math" / ".zpkg.toml",
              '[package]\norg = "acme"\nname = "math"\n')
        write(root / "package" / "zed_modules" / "acme" / "math" / "src" / "answer.ores",
              'pub fnc answer(): int { return 42; }\n')
        write(root / "package" / "src" / "pkg_main.ores",
              'import "pkg:math/src/answer";\npub fnc main(): int {return answer();}\n')
        expect_success(args.compiler, args.clang, root / "package" / "src" / "pkg_main.ores", required_calls=("answer",))
        write(root / "package" / "src" / "escape.ores", 'import "../../helper";\npub fnc main(): int {return add(40, 2);}\n')
        expect_failure(args.compiler, root / "package" / "src" / "escape.ores", "native import escapes physical search root")
        # Canonical search precedence: source.roots > source.import_paths
        # > ORES_PATHS > legacy ORESLANG_PATH. This uses real compiler IR.
        search = root / "search"
        write(search / ".ores.pkg.toml",
              'schema_version = "1"\n[source]\nroots = ["src"]\nimport_paths = ["vendor"]\n')
        write(search / "src" / "main.ores",
              'import "priority";\npub fnc main(): int {return priority();}\n')
        write(search / "src" / "priority.ores",
              'pub fnc priority(): int {return 42;}\n')
        write(search / "vendor" / "priority.ores",
              'pub fnc priority(): int {return 41;}\n')
        write(root / "ambient" / "priority.ores",
              'pub fnc priority(): int {return 40;}\n')
        write(root / "legacy" / "priority.ores",
              'pub fnc priority(): int {return 39;}\n')
        env = {"ORES_PATHS": str(root / "ambient"),
               "ORESLANG_PATH": str(root / "legacy")}
        entry = search / "src" / "main.ores"
        expect_success(args.compiler, args.clang, entry, 42, ("priority",), env)
        (search / "src" / "priority.ores").unlink()
        expect_success(args.compiler, args.clang, entry, 41, ("priority",), env)
        (search / "vendor" / "priority.ores").unlink()
        expect_success(args.compiler, args.clang, entry, 40, ("priority",), env)
        (root / "ambient" / "priority.ores").unlink()
        expect_success(args.compiler, args.clang, entry, 39, ("priority",), env)

        # A manifest's explicit roots=[] never silently adds its directory.
        write(search / "priority.ores",
              'pub fnc priority(): int {return 42;}\n')
        write(search / ".ores.pkg.toml",
              'schema_version = "1"\n[source]\nroots = []\nimport_paths = []\n')
        expect_failure(args.compiler, entry, "native scalar import source not found")
        expect_success(args.compiler, args.clang, entry, 42, ("priority",),
                       {"ORES_PATHS": str(search)})
        expect_failure(args.compiler, entry, "ORES_PATHS exceeds 64 KiB",
                       {"ORES_PATHS": "x" * 65537})

    print("native scalar import execution gates passed")

if __name__ == "__main__":
    main()
