#!/usr/bin/env python3
"""Closed native ores.core resolver and freestanding profile integration tests."""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def run(args):
    return subprocess.run(
        args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        text=True, timeout=60)


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang")
    a = p.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-core-resolver-") as tmp:
        root = pathlib.Path(tmp)
        source = root / "main.ores"
        source.write_text(
            'import module numeric from "ores.core/numeric"; '
            'pub fnc main():int{return numeric.max_int(40,42);}',
            encoding="utf-8",
        )

        host = run([a.compiler, str(source)])
        require(host.returncode == 0, host.stderr)
        require("__ores_module_1.max_int" in host.stdout,
                "compiler-owned numeric module missing from linked IR")

        if a.clang:
            ll = root / "main.ll"
            exe = root / "main"
            ll.write_text(host.stdout, encoding="utf-8")
            built = run([a.clang, str(ll), "-O2", "-o", str(exe)])
            require(built.returncode == 0, built.stderr)
            executed = run([str(exe)])
            require(executed.returncode == 42,
                    "host ores.core numeric execution returned "
                    + str(executed.returncode))

        # A project-controlled lookalike must not shadow the compiler registry.
        shadow_dir = root / "ores.core"
        shadow_dir.mkdir()
        (shadow_dir / "numeric.ores").write_text(
            "define module numeric as "
            "pub fnc max_int(int left,int right):int{return 7;} end",
            encoding="utf-8",
        )
        shadowed = run([a.compiler, str(source)])
        require(shadowed.returncode == 0, shadowed.stderr)
        require(shadowed.stdout == host.stdout,
                "project file shadowed compiler-owned ores.core module")

        unknown = root / "unknown.ores"
        unknown.write_text(
            'import module missing from "ores.core/missing"; '
            'pub fnc main():int{return 0;}',
            encoding="utf-8",
        )
        rejected = run([a.compiler, str(unknown)])
        require(rejected.returncode != 0 and not rejected.stdout,
                "unknown ores.core module was admitted")
        require("unknown compiler-owned ores.core module" in rejected.stderr,
                rejected.stderr)

        std = root / "std.ores"
        std.write_text(
            'import module net from "ores.std/net"; '
            'pub fnc main():int{return 0;}',
            encoding="utf-8",
        )
        rejected = run([a.compiler, str(std)])
        require(rejected.returncode != 0 and not rejected.stdout,
                "ores.std module unexpectedly resolved")
        require("ores.std native module resolution is not implemented"
                in rejected.stderr, rejected.stderr)

        legacy = root / "legacy.ores"
        legacy.write_text(
            'import module net from "std/net"; '
            'pub fnc main():int{return 0;}',
            encoding="utf-8",
        )
        rejected = run([a.compiler, str(legacy)])
        require(rejected.returncode != 0 and not rejected.stdout,
                "legacy std module unexpectedly resolved")
        require("ores.std native module resolution is not implemented"
                in rejected.stderr, rejected.stderr)

        # Explicit core must admit the compiler-owned module on both certified
        # no-OS targets without enabling relative/project filesystem imports.
        wasm = run([
            a.compiler,
            "--library-profile=core",
            "--target=wasm32-unknown-unknown",
            str(source),
        ])
        require(wasm.returncode == 0, wasm.stderr)
        require('target triple = "wasm32-unknown-unknown"' in wasm.stdout,
                "wasm core import did not reach target lowering")
        require("__ores_module_1.max_int" in wasm.stdout,
                "wasm core import missing linked compiler-owned function")

        cortex_source = root / "cortex.ores"
        cortex_source.write_text(
            'import module numeric from "ores.core/numeric"; '
            'pub fnc ores_embedded_probe(int x):int{'
            'return numeric.max_int(x,42);}',
            encoding="utf-8",
        )
        cortex = run([
            a.compiler,
            "--library-profile=core",
            "--target=thumbv7m-none-eabi",
            str(cortex_source),
        ])
        require(cortex.returncode == 0, cortex.stderr)
        require('target triple = "thumbv7m-none-eabi"' in cortex.stdout,
                "Cortex-M core import did not reach target lowering")
        require("__ores_module_1.max_int" in cortex.stdout,
                "Cortex-M core import missing linked compiler-owned function")

        relative = root / "relative.ores"
        (root / "local.ores").write_text(
            "define module local as pub fnc answer():int{return 42;} end",
            encoding="utf-8",
        )
        relative.write_text(
            'import module local from "./local"; '
            'pub fnc main():int{return local.answer();}',
            encoding="utf-8",
        )
        for target in ("wasm32-unknown-unknown", "thumbv7m-none-eabi"):
            rejected = run([
                a.compiler,
                "--library-profile=core",
                "--target=" + target,
                str(relative),
            ])
            require(rejected.returncode != 0 and not rejected.stdout,
                    target + " admitted ambient relative source import")
            require("non-core native module imports currently require"
                    in rejected.stderr, rejected.stderr)

    print("native closed ores.core resolver and freestanding import gates PASS")


if __name__ == "__main__":
    main()
