#!/usr/bin/env python3
"""End-to-end native ores.core registry and freestanding closure checks."""
import argparse
from pathlib import Path
import subprocess
import tempfile


def run(args, **kwargs):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=90,
        **kwargs,
    )


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def read_u32_leb(data, offset):
    value = 0
    shift = 0
    for _ in range(5):
        require(offset < len(data), "truncated wasm LEB128")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, offset
        shift += 7
    raise AssertionError("invalid wasm u32 LEB128")


def wasm_import_count(data):
    require(data[:8] == b"\x00asm\x01\x00\x00\x00", "not wasm v1")
    offset = 8
    while offset < len(data):
        section_id = data[offset]
        offset += 1
        size, payload = read_u32_leb(data, offset)
        end = payload + size
        require(end <= len(data), "truncated wasm section")
        if section_id == 2:
            count, _ = read_u32_leb(data, payload)
            return count
        offset = end
    return 0


def require_rejected(result, diagnostic):
    require(result.returncode != 0, "unexpected admission")
    require(not result.stdout, "rejected source emitted IR")
    require(diagnostic in result.stderr, (diagnostic, result.stderr))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang")
    parser.add_argument("--nm")
    parser.add_argument("--lld")
    parser.add_argument("--wasm-ld")
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-core-registry-") as temp:
        root = Path(temp)

        # A user-controlled filesystem path matching the canonical namespace
        # must never shadow compiler-owned source.
        shadow = root / "ores.core"
        shadow.mkdir()
        (shadow / "numeric.ores").write_text(
            "define module numeric as "
            "pub fnc clamp_int(int value,int lower,int upper):int{return 7;} "
            "end\n",
            encoding="utf-8",
        )

        main_source = root / "main.ores"
        main_source.write_text(
            'import module numeric from "ores.core/numeric"; '
            "pub fnc main():int{return numeric.clamp_int(50,0,42);}\n",
            encoding="utf-8",
        )
        host = run([args.compiler, str(main_source)])
        require(host.returncode == 0, host.stderr)
        require("@__ores_module_1.clamp_int" in host.stdout, "core module not linked")
        require("__ores_module_2.clamp_int" not in host.stdout, "duplicate core identity")

        if args.clang:
            ll = root / "host.ll"
            exe = root / "host"
            ll.write_text(host.stdout, encoding="utf-8")
            built = run([args.clang, str(ll), "-O2", "-o", str(exe)])
            require(built.returncode == 0, built.stderr)
            executed = run([str(exe)], env={"PATH": "/nonexistent"})
            require(
                executed.returncode == 42,
                "filesystem shadow replaced compiler-owned numeric source",
            )

        # Optional .ores spelling must resolve to exactly the same unit identity.
        alias_source = root / "alias.ores"
        alias_source.write_text(
            'import module numeric as A from "ores.core/numeric"; '
            'import module numeric as B from "ores.core/numeric.ores"; '
            "pub fnc main():int{return A.max_int(B.min_int(42,99),40);}\n",
            encoding="utf-8",
        )
        alias = run([args.compiler, str(alias_source)])
        require(alias.returncode == 0, alias.stderr)
        require("__ores_module_2." not in alias.stdout, "core alias duplicated module")

        atto_source = root / "canonical.atto"
        atto_source.write_text(
            'import module numeric from "atto.core/numeric.atto"; '
            "pub fnc main():int{return numeric.clamp_int(50,0,42);}\n",
            encoding="utf-8",
        )
        canonical = run([args.compiler, str(atto_source)])
        require(canonical.returncode == 0, canonical.stderr)
        require("@__ores_module_1.clamp_int" in canonical.stdout,
                "canonical atto.core module missing from emitted IR")

        unknown_source = root / "unknown.ores"
        unknown_source.write_text(
            'import module missing from "ores.core/missing"; '
            "pub fnc main():int{return 0;}\n",
            encoding="utf-8",
        )
        require_rejected(
            run([args.compiler, str(unknown_source)]),
            "unknown compiler-owned ores.core module",
        )

        traversal_source = root / "traversal.ores"
        traversal_source.write_text(
            'import module numeric from "ores.core/../numeric"; '
            "pub fnc main():int{return 0;}\n",
            encoding="utf-8",
        )
        require_rejected(
            run([args.compiler, str(traversal_source)]),
            "unknown compiler-owned ores.core module",
        )

        std_source = root / "std.ores"
        std_source.write_text(
            'import module testing from "ores.std/testing"; '
            "pub fnc main():int{return 0;}\n",
            encoding="utf-8",
        )
        require_rejected(
            run([args.compiler, str(std_source)]),
            "native ores.std package resolution remains unsupported",
        )

        relative_source = root / "relative.ores"
        relative_source.write_text(
            'import module local from "./local"; '
            "pub fnc probe(int x):int{return local.answer(x);}\n",
            encoding="utf-8",
        )
        (root / "local.ores").write_text(
            "define module local as "
            "pub fnc answer(int x):int{return x;} end\n",
            encoding="utf-8",
        )
        require_rejected(
            run([
                args.compiler,
                "--target=thumbv7m-none-eabi",
                "--library-profile=core",
                str(relative_source),
            ]),
            "freestanding native imports require compiler-owned ores.core/* modules",
        )

        cortex_source = root / "cortex.ores"
        cortex_source.write_text(
            'import module numeric from "ores.core/numeric"; '
            "pub fnc ores_embedded_probe(int x):int{"
            "return numeric.clamp_int(x,0,42);}\n",
            encoding="utf-8",
        )
        cortex = run([
            args.compiler,
            "--target=thumbv7m-none-eabi",
            "--library-profile=core",
            str(cortex_source),
        ])
        require(cortex.returncode == 0, cortex.stderr)
        require('target triple = "thumbv7m-none-eabi"' in cortex.stdout, "bad ARM triple")
        require("@__ores_module_1.clamp_int" in cortex.stdout, "ARM core import missing")

        wasm_source = root / "wasm.ores"
        wasm_source.write_text(
            'import module numeric from "ores.core/numeric.ores"; '
            "pub fnc main():int{return numeric.max_int(40,42);}\n",
            encoding="utf-8",
        )
        wasm = run([
            args.compiler,
            "--target=wasm32-unknown-unknown",
            "--library-profile=core",
            str(wasm_source),
        ])
        require(wasm.returncode == 0, wasm.stderr)
        require('target triple = "wasm32-unknown-unknown"' in wasm.stdout, "bad wasm triple")
        require("@__ores_module_1.max_int" in wasm.stdout, "wasm core import missing")

        artifact_tools = [args.clang, args.nm, args.lld, args.wasm_ld]
        if any(artifact_tools):
            require(all(artifact_tools), "artifact proof requires clang, nm, lld, wasm-ld")

            cortex_ll = root / "cortex.ll"
            cortex_obj = root / "cortex.o"
            cortex_elf = root / "cortex.elf"
            cortex_ll.write_text(cortex.stdout, encoding="utf-8")
            built = run([
                args.clang,
                "--target=thumbv7m-none-eabi",
                "-mcpu=cortex-m3",
                "-mthumb",
                "-O2",
                "-ffreestanding",
                "-fno-unwind-tables",
                "-fno-asynchronous-unwind-tables",
                "-x",
                "ir",
                "-c",
                str(cortex_ll),
                "-o",
                str(cortex_obj),
            ])
            require(built.returncode == 0, built.stderr)
            undefined = run([args.nm, "-u", str(cortex_obj)])
            require(undefined.returncode == 0, undefined.stderr)
            require(not undefined.stdout.strip(), undefined.stdout)
            linked = run([
                args.lld,
                "-m",
                "armelf",
                "-e",
                "ores_embedded_probe",
                "-o",
                str(cortex_elf),
                str(cortex_obj),
            ])
            require(linked.returncode == 0, linked.stderr)
            undefined = run([args.nm, "-u", str(cortex_elf)])
            require(undefined.returncode == 0, undefined.stderr)
            require(not undefined.stdout.strip(), undefined.stdout)

            wasm_ll = root / "wasm.ll"
            wasm_obj = root / "wasm.o"
            wasm_bin = root / "core.wasm"
            wasm_ll.write_text(wasm.stdout, encoding="utf-8")
            built = run([
                args.clang,
                "--target=wasm32-unknown-unknown",
                "-O2",
                "-ffreestanding",
                "-x",
                "ir",
                "-c",
                str(wasm_ll),
                "-o",
                str(wasm_obj),
            ])
            require(built.returncode == 0, built.stderr)
            linked = run([
                args.wasm_ld,
                "--no-entry",
                "--export=main",
                "-o",
                str(wasm_bin),
                str(wasm_obj),
            ])
            require(linked.returncode == 0, linked.stderr)
            require(
                wasm_import_count(wasm_bin.read_bytes()) == 0,
                "ores.core wasm artifact gained host imports",
            )

    print("native compiler-owned ores.core registry and closure gates PASS")


if __name__ == "__main__":
    main()
