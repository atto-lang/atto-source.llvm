#!/usr/bin/env python3
"""Compiler-owned alloc/* provider and transitive closure integration tests."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=90,
        check=False,
    )


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reject(result, diagnostic):
    require(result.returncode != 0, "unexpected compiler admission")
    require(not result.stdout, "rejected source emitted LLVM IR")
    require(diagnostic in result.stderr, (diagnostic, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang")
    args = parser.parse_args()
    if args.clang is not None and not args.clang.strip():
        raise ValueError("native guest-link evidence requires a nonempty Clang command")

    source_root = Path(__file__).resolve().parents[1]
    runtime_c = (
        source_root
        / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    )
    include = source_root / "src/main/cpp"

    with tempfile.TemporaryDirectory(prefix="ores-alloc-library-") as tmp:
        root = Path(tmp)

        # A user-controlled lookalike must never shadow the compiler registry.
        shadow = root / "alloc"
        shadow.mkdir()
        (shadow / "int_buffer.ores").write_text(
            "define module int_buffer as "
            "pub fnc sum_pair(int left,int right):int{return 7;} end\n",
            encoding="utf-8",
        )

        main_source = root / "main.ores"
        main_source.write_text(
            'import module int_buffer from "alloc/int_buffer"; '
            "pub fnc main():int{return int_buffer.sum_pair(19,23);}\n",
            encoding="utf-8",
        )
        compiled = run([args.compiler, str(main_source)])
        require(compiled.returncode == 0, compiled.stderr)
        require(
            "define i64 @__ores_module_1.sum_pair" in compiled.stdout,
            "compiler-owned alloc/int_buffer was not linked",
        )
        require(
            "@oreslang_int_array_try_new" in compiled.stdout,
            "allocator-backed core-profile facility emitted no allocator ABI",
        )

        if args.clang:
            require(runtime_c.is_file(), f"missing runtime source: {runtime_c}")
            ll = root / "main.ll"
            exe = root / "main"
            ll.write_text(compiled.stdout, encoding="utf-8")
            linked = run([
                args.clang,
                "-O2",
                "-Wno-override-module",
                "-std=c11",
                "-x",
                "ir",
                str(ll),
                "-x",
                "c",
                str(runtime_c),
                "-I",
                str(include),
                "-o",
                str(exe),
            ])
            require(linked.returncode == 0, linked.stderr)
            executed = run([str(exe)])
            require(
                executed.returncode == 42,
                f"allocator-backed module returned {executed.returncode}",
            )

        # Optional .ores spelling must keep one compiler-owned unit identity.
        alias_source = root / "alias.ores"
        alias_source.write_text(
            'import module int_buffer as A from "alloc/int_buffer"; '
            'import module int_buffer as B from "alloc/int_buffer.ores"; '
            "pub fnc main():int{return A.sum_pair(10,11)+B.sum_pair(10,11);}\n",
            encoding="utf-8",
        )
        alias = run([args.compiler, str(alias_source)])
        require(alias.returncode == 0, alias.stderr)
        require(
            alias.stdout.count("define i64 @__ores_module_1.sum_pair") == 1,
            "alloc alias duplicated canonical compiler-owned module",
        )
        require(
            "__ores_module_2.sum_pair" not in alias.stdout,
            "alloc alias created a second compiler-owned unit identity",
        )

        # New AttoLang source and canonical atto.alloc imports must share
        # one compiler-owned module with legacy .ores aliases. Prove this by
        # IR identity as well as by compiling and executing native code.
        modern_source = root / "modern-alloc.atto"
        modern_source.write_text(
            'import module int_buffer as New from "atto.alloc/int_buffer.atto"; '
            'import module int_buffer as Old from "alloc/int_buffer.ores"; '
            "pub fnc main():int{return New.sum_pair(10,11)"
            "+Old.sum_pair(10,11);}\n",
            encoding="utf-8",
        )
        modern = run([args.compiler, str(modern_source)])
        require(modern.returncode == 0, modern.stderr)
        require(
            modern.stdout.count("define i64 @__ores_module_1.sum_pair") == 1
            and "__ores_module_2.sum_pair" not in modern.stdout,
            "canonical .atto and legacy .ores alloc imports created duplicates",
        )
        require("@oreslang_int_array_try_new" in modern.stdout,
                "modern alloc alias lost the native allocator ABI")
        if args.clang:
            ll = root / "modern-alloc.ll"
            exe = root / "modern-alloc"
            ll.write_text(modern.stdout, encoding="utf-8")
            linked = run([
                args.clang, "-O2", "-Wno-override-module", "-std=c11",
                "-x", "ir", str(ll), "-x", "c", str(runtime_c),
                "-I", str(include), "-o", str(exe),
            ])
            require(linked.returncode == 0, linked.stderr)
            executed = run([str(exe)])
            require(executed.returncode == 42,
                    "compiled .atto alloc aliases did not execute to 42")

        # The bundle's own body allocates nothing. Its dependency does, so the
        # linked artifact proves the loader followed and retained the transitive
        # provider-bearing closure.
        bundle_source = root / "bundle.ores"
        bundle_source.write_text(
            'import module int_buffer_bundle as bundle from "alloc/int_buffer_bundle"; '
            "pub fnc main():int{return bundle.marker();}\n",
            encoding="utf-8",
        )
        bundle = run([args.compiler, str(bundle_source)])
        require(bundle.returncode == 0, bundle.stderr)
        require(
            "define i64 @__ores_module_1.marker" in bundle.stdout,
            "alloc bundle module missing",
        )
        require(
            "define i64 @__ores_module_2.sum_pair" in bundle.stdout,
            "transitive alloc dependency missing from linked closure",
        )
        require(
            "@oreslang_int_array_try_new" in bundle.stdout,
            "transitive provider-bearing dependency lost allocator ABI",
        )

        modern_bundle_source = root / "modern-bundle.atto"
        modern_bundle_source.write_text(
            'import module int_buffer_bundle as bundle '
            'from "atto.alloc/int_buffer_bundle.atto"; '
            "pub fnc main():int{return bundle.marker();}\n",
            encoding="utf-8",
        )
        modern_bundle = run([args.compiler, str(modern_bundle_source)])
        require(modern_bundle.returncode == 0, modern_bundle.stderr)
        require(
            "define i64 @__ores_module_1.marker" in modern_bundle.stdout and
            "define i64 @__ores_module_2.sum_pair" in modern_bundle.stdout,
            "canonical .atto bundle lost its transitive allocator dependency",
        )
        require("@oreslang_int_array_try_new" in modern_bundle.stdout,
                "canonical .atto transitive allocation lost provider gate")

        if args.clang:
            bundle_ll = root / "modern-bundle.ll"
            bundle_exe = root / "modern-bundle"
            bundle_ll.write_text(modern_bundle.stdout, encoding="utf-8")
            linked = run([
                args.clang, "-O2", "-Wno-override-module", "-std=c11",
                "-x", "ir", str(bundle_ll), "-x", "c", str(runtime_c),
                "-I", str(include), "-o", str(bundle_exe),
            ])
            require(linked.returncode == 0, linked.stderr)
            executed = run([str(bundle_exe)])
            require(executed.returncode == 42,
                    "compiled .atto transitive alloc bundle did not execute to 42")

        modern_unknown = root / "atto-alloc-unknown.atto"
        modern_unknown.write_text(
            'import module missing from "atto.alloc/missing"; '
            "pub fnc main():int{return 0;}\n", encoding="utf-8")
        reject(run([args.compiler, str(modern_unknown)]),
               "unknown compiler-owned atto.alloc module")
        modern_traversal = root / "atto-alloc-traversal.atto"
        modern_traversal.write_text(
            'import module int_buffer from "atto.alloc/../int_buffer"; '
            "pub fnc main():int{return 0;}\n", encoding="utf-8")
        reject(run([args.compiler, str(modern_traversal)]),
               "unknown compiler-owned atto.alloc module")

        invalid_suffix = root / "invalid-alloc-suffix.atto"
        invalid_suffix.write_text(
            'import module int_buffer from "alloc/int_buffer.txt"; '
            "pub fnc main():int{return 0;}\n", encoding="utf-8")
        reject(run([args.compiler, str(invalid_suffix)]),
               "unknown compiler-owned alloc module")

        unknown = root / "unknown.ores"
        unknown.write_text(
            'import module missing from "alloc/missing"; '
            "pub fnc main():int{return 0;}\n",
            encoding="utf-8",
        )
        reject(
            run([args.compiler, str(unknown)]),
            "unknown compiler-owned alloc module",
        )

        traversal = root / "traversal.ores"
        traversal.write_text(
            'import module int_buffer from "alloc/../int_buffer"; '
            "pub fnc main():int{return 0;}\n",
            encoding="utf-8",
        )
        reject(
            run([args.compiler, str(traversal)]),
            "unknown compiler-owned alloc module",
        )

        # No freestanding allocator provider ABI exists yet. Fail before IR,
        # rather than silently binding to libc or an ambient runtime allocator.
        for target in ("wasm32-unknown-unknown", "thumbv7m-none-eabi"):
            for source in (main_source, modern_source, modern_bundle_source):
                rejected = run([
                    args.compiler,
                    "--library-profile=core",
                    "--target=" + target,
                    str(source),
                ])
                reject(
                    rejected,
                    "freestanding alloc/* requires an explicit allocator provider",
                )

    print(
        "compiler-owned alloc tier, allocator ABI, alias identity, "
        "transitive closure, and freestanding provider gates PASS"
    )


if __name__ == "__main__":
    main()
