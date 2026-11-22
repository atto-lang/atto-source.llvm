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

        canonical_source = root / "canonical.atto"
        canonical_source.write_text(
            'import module int_buffer from "atto.alloc/int_buffer.atto"; '
            "pub fnc main():int{return int_buffer.sum_pair(19,23);}\n",
            encoding="utf-8",
        )
        canonical = run([args.compiler, str(canonical_source)])
        require(canonical.returncode == 0, canonical.stderr)
        require("define i64 @__ores_module_1.sum_pair" in canonical.stdout,
                "canonical atto.alloc module was not compiled")

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

        mixed_source = root / "mixed_alias.atto"
        mixed_source.write_text(
            'import module int_buffer as A from "atto.alloc/int_buffer"; '
            'import module int_buffer as B from "alloc/int_buffer.ores"; '
            "pub fnc main():int{return A.sum_pair(1,2)+B.sum_pair(3,4);}\n",
            encoding="utf-8",
        )
        mixed = run([args.compiler, str(mixed_source)])
        require(mixed.returncode == 0, mixed.stderr)
        require(mixed.stdout.count("define i64 @__ores_module_1.sum_pair") == 1,
                "canonical and legacy alloc paths duplicated unit identity")

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

        # Real Atto guest compile/link/run: new compiler-owned pure alloc
        # modules must resolve to embedded trusted sources, not shadow files.
        # These policies do NOT claim physical allocation or Drop execution.
        alloc_fixture = source_root / "tests/fixtures/alloc-heap-arena-region-policy.atto"
        require(alloc_fixture.is_file(), "missing allocation policy guest fixture")
        for name in ("heap", "arena", "region"):
            (shadow / (name + ".atto")).write_text(
                'define module ' + name +
                ' as pub fnc forged():int{return 99;} end\\n',
                encoding="utf-8",
            )
        pure = run([args.compiler, str(alloc_fixture)])
        require(pure.returncode == 0, "alloc policy guest rejected: " + pure.stderr)
        for symbol in ("heap_reserved_total", "arena_aligned_start", "region_can_destroy"):
            require(
                "define " in pure.stdout and symbol in pure.stdout,
                "native compiler did not emit policy function " + symbol,
            )
        if args.clang:
            pure_ir = root / "alloc_policy.ll"
            pure_exe = root / "alloc_policy"
            pure_ir.write_text(pure.stdout, encoding="utf-8")
            pure_link = run([
                args.clang, "-O2", "-Wno-override-module",
                "-x", "ir", str(pure_ir), "-o", str(pure_exe)
            ])
            require(pure_link.returncode == 0, pure_link.stderr)
            pure_run = run([str(pure_exe)])
            require(
                pure_run.returncode == 42,
                "compiled Atto alloc policy guest returned " +
                str(pure_run.returncode) + ": " + pure_run.stderr,
            )
        alias_policy = root / "alloc_alias.atto"
        alias_policy.write_text(
            'import module heap as H from "atto.alloc/heap"; '
            'import module heap as L from "alloc/heap.ores"; '
            'pub fnc main():int{'
            'if H.heap_reserved_total(2,3,4,10) == 9 { '
            'return L.heap_reserved_total(2,3,4,10)+33; } '
            'return 1;}\\n',
            encoding="utf-8",
        )
        alias_compiled = run([args.compiler, str(alias_policy)])
        require(alias_compiled.returncode == 0, alias_compiled.stderr)
        require(
            alias_compiled.stdout.count("define i64 @__ores_module_1.heap_reserved_total") == 1,
            "alloc/heap spellings duplicated the compiler-owned module",
        )
        require(
            "__ores_module_2.heap_reserved_total" not in alias_compiled.stdout,
            "same alloc/heap imported as second module",
        )

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
            rejected = run([
                args.compiler,
                "--library-profile=core",
                "--target=" + target,
                str(main_source),
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
