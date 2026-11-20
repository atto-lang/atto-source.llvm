#!/usr/bin/env python3
"""Exact native artifact gate for the public core library profile.

This test exercises the production compiler with an explicit core profile,
then proves that the resulting Cortex-M and wasm32 artifacts do not acquire
ambient hosted dependencies.
"""
import argparse
import pathlib
import shutil
import subprocess
import tempfile


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def find_tool(*names):
    for name in names:
        path = shutil.which(name)
        if path:
            return path
    return None


def run(args, timeout=90):
    return subprocess.run(
        args,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )


def read_u32_leb(data, offset):
    value = 0
    shift = 0
    for _ in range(5):
        require(offset < len(data), "truncated wasm LEB128")
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7F) << shift
        if (byte & 0x80) == 0:
            return value, offset
        shift += 7
    raise RuntimeError("invalid wasm u32 LEB128")


def wasm_import_count(data):
    require(
        data[:8] == b"\x00asm\x01\x00\x00\x00",
        "linked artifact is not WebAssembly v1",
    )
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


def require_no_undefined(nm, artifact):
    result = run([nm, "-u", str(artifact)])
    require(result.returncode == 0, result.stderr)
    require(
        not result.stdout.strip(),
        "freestanding artifact has undefined symbols:\n" + result.stdout,
    )


def compile_core_ir(compiler, target, source):
    explicit = run([
        compiler,
        "--target=" + target,
        "--library-profile=core",
        str(source),
    ])
    require(explicit.returncode == 0, explicit.stderr)

    default = run([
        compiler,
        "--target=" + target,
        str(source),
    ])
    require(default.returncode == 0, default.stderr)
    require(
        default.stdout == explicit.stdout,
        "explicit core profile changed the certified target artifact",
    )

    hosted = run([
        compiler,
        "--target=" + target,
        "--library-profile=std",
        str(source),
    ])
    require(
        hosted.returncode != 0 and not hosted.stdout,
        "freestanding target incorrectly admitted ores.std",
    )
    return explicit.stdout


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()

    clang = find_tool("clang", "clang-20", "clang-19", "clang-18")
    nm = find_tool("llvm-nm", "llvm-nm-20", "llvm-nm-19", "llvm-nm-18", "nm")
    lld = find_tool("ld.lld", "ld.lld-20", "ld.lld-19", "ld.lld-18")
    wasm_ld = find_tool("wasm-ld", "wasm-ld-20", "wasm-ld-19", "wasm-ld-18")
    require(clang, "clang is required for freestanding artifact validation")
    require(nm, "llvm-nm or nm is required for undefined-symbol validation")
    require(lld, "ld.lld is required for Cortex-M no-stdlib linking")
    require(wasm_ld, "wasm-ld is required for wasm32 no-host-import linking")

    with tempfile.TemporaryDirectory(prefix="ores-core-artifact-") as tmp:
        root = pathlib.Path(tmp)

        cortex_source = root / "cortex.ores"
        cortex_source.write_text(
            "pub fnc ores_embedded_probe(int a, int b): int {\n"
            "  return a + b;\n"
            "}\n",
            encoding="utf-8",
        )
        cortex_ir = compile_core_ir(
            args.compiler,
            "thumbv7m-none-eabi",
            cortex_source,
        )
        require(
            'target triple = "thumbv7m-none-eabi"' in cortex_ir,
            "Cortex-M target triple missing",
        )

        cortex_ll = root / "cortex.ll"
        cortex_obj = root / "cortex.o"
        cortex_elf = root / "cortex.elf"
        cortex_ll.write_text(cortex_ir, encoding="utf-8")

        compiled = run([
            clang,
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
        require(compiled.returncode == 0, compiled.stderr)
        require_no_undefined(nm, cortex_obj)

        linked = run([
            lld,
            "-m",
            "armelf",
            "-e",
            "ores_embedded_probe",
            "-o",
            str(cortex_elf),
            str(cortex_obj),
        ])
        require(linked.returncode == 0, linked.stderr)
        elf = cortex_elf.read_bytes()
        require(
            len(elf) >= 52
            and elf[:4] == b"\x7fELF"
            and elf[4:6] == b"\x01\x01",
            "linked Cortex-M artifact is not ELF32 little-endian",
        )
        require(
            int.from_bytes(elf[18:20], "little") == 40,
            "linked Cortex-M artifact is not ARM ELF",
        )
        require_no_undefined(nm, cortex_elf)

        wasm_source = root / "wasm.ores"
        wasm_source.write_text(
            "pub fnc main(): int { return 42; }\n",
            encoding="utf-8",
        )
        wasm_ir = compile_core_ir(
            args.compiler,
            "wasm32-unknown-unknown",
            wasm_source,
        )
        require(
            'target triple = "wasm32-unknown-unknown"' in wasm_ir,
            "wasm32 target triple missing",
        )

        wasm_ll = root / "guest.ll"
        wasm_obj = root / "guest.o"
        wasm_bin = root / "guest.wasm"
        wasm_ll.write_text(wasm_ir, encoding="utf-8")

        compiled = run([
            clang,
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
        require(compiled.returncode == 0, compiled.stderr)

        linked = run([
            wasm_ld,
            "--no-entry",
            "--export=main",
            "-o",
            str(wasm_bin),
            str(wasm_obj),
        ])
        require(linked.returncode == 0, linked.stderr)
        wasm = wasm_bin.read_bytes()
        require(
            wasm_import_count(wasm) == 0,
            "core wasm artifact contains ambient host imports",
        )

    print(
        "ores.core Cortex-M ELF and wasm32 no-host-dependency artifact gates PASS"
    )


if __name__ == "__main__":
    main()
