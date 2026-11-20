#!/usr/bin/env python3
"""Freestanding AllocatorProviderV1 target integration.

Proves explicit provider admission, no-libc provider/runtime linking, zero Wasm
host imports, real Wasm execution, and a Cortex-M no-undefined-symbol artifact.
QEMU Cortex execution is performed when --qemu-system-arm is supplied.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(args, timeout=90):
    return subprocess.run(
        [str(x) for x in args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
        check=False,
    )


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def reject(result, text):
    require(result.returncode != 0, "unexpected admission")
    require(not result.stdout, "rejected compilation emitted IR")
    require(text in result.stderr, (text, result.stderr))


def find_tool(explicit, *names):
    if explicit:
        path = Path(explicit)
        require(path.is_file(), f"missing explicit tool: {path}")
        # Preserve argv[0] for multi-call linker binaries such as lld.
        # Resolving wasm-ld/ld.lld symlinks to generic lld loses target
        # personality selection and makes an otherwise valid proof fail.
        return str(path)
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    return None


def read_uleb(data, offset):
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
    require(data[:8] == b"\x00asm\x01\x00\x00\x00",
            "linked artifact is not WebAssembly v1")
    offset = 8
    while offset < len(data):
        section_id = data[offset]
        offset += 1
        size, payload = read_uleb(data, offset)
        end = payload + size
        require(end <= len(data), "truncated wasm section")
        if section_id == 2:
            count, _ = read_uleb(data, payload)
            return count
        offset = end
    return 0


def compile_c(clang, target, source, output, include, extra=()):
    command = [
        clang,
        "--target=" + target,
        "-std=c11",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-O0",
        "-I",
        include,
        "-c",
        source,
        "-o",
        output,
    ]
    command[2:2] = list(extra)
    result = run(command)
    require(result.returncode == 0, result.stderr)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--source-root", required=True)
    p.add_argument("--clang")
    p.add_argument("--wasm-ld")
    p.add_argument("--llvm-nm")
    p.add_argument("--node")
    p.add_argument("--qemu-system-arm")
    args = p.parse_args()

    source_root = Path(args.source_root).resolve()
    include = str(source_root / "src/main/cpp")
    provider_c = source_root / "src/main/cpp/oreslang/runtime/AllocatorProviderV1.c"
    array_c = source_root / "src/main/cpp/oreslang/runtime/FreestandingIntArrayRegion.c"
    for path in (provider_c, array_c):
        require(path.is_file(), f"missing freestanding runtime source: {path}")

    clang = find_tool(args.clang, "clang-18", "clang-19", "clang")
    wasm_ld = find_tool(args.wasm_ld, "wasm-ld-18", "wasm-ld-19", "wasm-ld")
    llvm_nm = find_tool(args.llvm_nm, "llvm-nm-18", "llvm-nm-19", "llvm-nm")
    llvm_readelf = find_tool(None, "llvm-readelf-18", "llvm-readelf-19", "llvm-readelf")
    node = find_tool(args.node, "node")
    qemu = find_tool(args.qemu_system_arm, "qemu-system-arm")
    require(clang, "clang is required")
    require(wasm_ld, "wasm-ld is required")
    require(llvm_nm, "llvm-nm is required")
    require(llvm_readelf, "llvm-readelf is required")

    with tempfile.TemporaryDirectory(prefix="ores-allocator-provider-v1-") as tmp:
        root = Path(tmp)
        wasm_source = root / "wasm.ores"
        wasm_source.write_text(
            'import module int_buffer from "alloc/int_buffer"; '
            "pub fnc main():int{return int_buffer.sum_pair(19,23);}\n",
            encoding="utf-8",
        )
        cortex_source = root / "cortex.ores"
        cortex_source.write_text(
            'import module int_buffer from "alloc/int_buffer"; '
            "pub fnc ores_embedded_probe():int{"
            "return int_buffer.sum_pair(19,23);}\n",
            encoding="utf-8",
        )

        # Missing/invalid/ambient provider selection is fail-closed.
        for target, source in (
            ("wasm32-unknown-unknown", wasm_source),
            ("thumbv7m-none-eabi", cortex_source),
        ):
            reject(
                run([
                    args.compiler,
                    "--library-profile=core",
                    "--target=" + target,
                    str(source),
                ]),
                "freestanding alloc/* requires an explicit allocator provider",
            )
        reject(
            run([
                args.compiler,
                "--allocator-provider=ambient",
                "--target=wasm32-unknown-unknown",
                str(wasm_source),
            ]),
            "unsupported allocator provider",
        )
        reject(
            run([
                args.compiler,
                "--allocator-provider=bounded-arena",
                str(wasm_source),
            ]),
            "bounded-arena allocator provider is certified only for freestanding targets",
        )

        common = [
            args.compiler,
            "--library-profile=core",
            "--allocator-provider=bounded-arena",
        ]

        wasm_ir = run(common + [
            "--target=wasm32-unknown-unknown",
            str(wasm_source),
        ])
        require(wasm_ir.returncode == 0, wasm_ir.stderr)
        require('target triple = "wasm32-unknown-unknown"' in wasm_ir.stdout,
                "wasm allocator program missing target triple")
        require("@oreslang_int_array_try_region_open" in wasm_ir.stdout,
                "wasm allocator program lost provider-backed Array ABI")
        wasm_ll = root / "guest.ll"
        wasm_ll.write_text(wasm_ir.stdout, encoding="utf-8")
        wasm_obj = root / "guest-wasm.o"
        compiled = run([
            clang,
            "--target=wasm32-unknown-unknown",
            "-ffreestanding",
            "-fno-builtin",
            "-fno-unwind-tables",
            "-fno-asynchronous-unwind-tables",
            "-O1",
            "-x",
            "ir",
            "-c",
            str(wasm_ll),
            "-o",
            str(wasm_obj),
        ])
        require(compiled.returncode == 0, compiled.stderr)

        wasm_shim = root / "wasm-shim.c"
        wasm_shim.write_text(
            '#include "oreslang/runtime/AllocatorProviderV1.h"\n'
            "#include <stdint.h>\n"
            "extern long long main(void);\n"
            "static union { uint64_t align; unsigned char bytes[32768]; } arena;\n"
            "static union { uint64_t align; unsigned char bytes[128]; } tiny_arena;\n"
            "static OresAllocatorProviderV1 provider;\n"
            "int run_success(void) {\n"
            "  if (ores_allocator_provider_v1_init(&provider, arena.bytes, sizeof(arena.bytes))) return 90;\n"
            "  if (ores_allocator_provider_v1_bind(&provider)) return 91;\n"
            "  int result = (int)main();\n"
            "  if (ores_allocator_provider_v1_used(&provider) == 0) return 92;\n"
            "  if (ores_allocator_provider_v1_unbind(&provider)) return 93;\n"
            "  if (ores_allocator_provider_v1_reset(&provider)) return 94;\n"
            "  if (ores_allocator_provider_v1_used(&provider) != 0) return 95;\n"
            "  return result;\n"
            "}\n"
            "int run_tiny(void) {\n"
            "  if (ores_allocator_provider_v1_init(&provider, tiny_arena.bytes, sizeof(tiny_arena.bytes))) return 96;\n"
            "  if (ores_allocator_provider_v1_bind(&provider)) return 97;\n"
            "  return (int)main();\n"
            "}\n",
            encoding="utf-8",
        )

        wasm_parts = []
        for name, source in (
            ("provider", provider_c),
            ("array-runtime", array_c),
            ("shim", wasm_shim),
        ):
            obj = root / (name + "-wasm.o")
            compile_c(
                clang, "wasm32-unknown-unknown",
                str(source), str(obj), include,
            )
            wasm_parts.append(obj)

        wasm_bin = root / "provider.wasm"
        linked = run([
            wasm_ld,
            "--no-entry",
            "--export=run_success",
            "--export=run_tiny",
            "--export-memory",
            "-o",
            str(wasm_bin),
            str(wasm_obj),
            *map(str, wasm_parts),
        ])
        require(linked.returncode == 0, linked.stderr)
        wasm_bytes = wasm_bin.read_bytes()
        require(wasm_import_count(wasm_bytes) == 0,
                "provider-backed wasm artifact has ambient host imports")

        if node:
            js = root / "run-wasm.js"
            js.write_text(
                "const fs=require('fs');\n"
                "const b=fs.readFileSync(process.argv[2]);\n"
                "(async()=>{\n"
                " const m=await WebAssembly.instantiate(b,{});\n"
                " if(WebAssembly.Module.imports(m.module).length!==0) process.exit(20);\n"
                " if(m.instance.exports.run_success()!==42) process.exit(21);\n"
                " const n=await WebAssembly.instantiate(b,{});\n"
                " let trapped=false; try{n.instance.exports.run_tiny();}catch(e){trapped=true;}\n"
                " if(!trapped) process.exit(22);\n"
                " process.exit(0);\n"
                "})().catch(()=>process.exit(23));\n",
                encoding="utf-8",
            )
            executed = run([node, str(js), str(wasm_bin)])
            require(executed.returncode == 0,
                    f"wasm provider execution failed: {executed.stderr}")

        cortex_ir = run(common + [
            "--target=thumbv7m-none-eabi",
            str(cortex_source),
        ])
        require(cortex_ir.returncode == 0, cortex_ir.stderr)
        require('target triple = "thumbv7m-none-eabi"' in cortex_ir.stdout,
                "Cortex allocator program missing target triple")
        cortex_ll = root / "cortex.ll"
        cortex_ll.write_text(cortex_ir.stdout, encoding="utf-8")
        cortex_obj = root / "guest-cortex.o"
        compiled = run([
            clang,
            "--target=thumbv7m-none-eabi",
            "-mcpu=cortex-m3",
            "-mthumb",
            "-ffreestanding",
            "-fno-builtin",
            "-O1",
            "-x",
            "ir",
            "-c",
            str(cortex_ll),
            "-o",
            str(cortex_obj),
        ])
        require(compiled.returncode == 0, compiled.stderr)

        cortex_shim = root / "cortex-shim.c"
        cortex_shim.write_text(
            '#include "oreslang/runtime/AllocatorProviderV1.h"\n'
            "#include <stdint.h>\n"
            "extern long long ores_embedded_probe(void);\n"
            "static union { uint64_t align; unsigned char bytes[32768]; } arena;\n"
            "static OresAllocatorProviderV1 provider;\n"
            "int allocator_probe(void) {\n"
            "  if (ores_allocator_provider_v1_init(&provider, arena.bytes, sizeof(arena.bytes))) return 90;\n"
            "  if (ores_allocator_provider_v1_bind(&provider)) return 91;\n"
            "  int result=(int)ores_embedded_probe();\n"
            "  if (ores_allocator_provider_v1_used(&provider)==0) return 92;\n"
            "  if (ores_allocator_provider_v1_unbind(&provider)) return 93;\n"
            "  if (ores_allocator_provider_v1_reset(&provider)) return 94;\n"
            "  return result;\n"
            "}\n",
            encoding="utf-8",
        )
        cortex_parts = []
        for name, source in (
            ("provider", provider_c),
            ("array-runtime", array_c),
            ("shim", cortex_shim),
        ):
            obj = root / (name + "-cortex.o")
            compile_c(
                clang, "thumbv7m-none-eabi",
                str(source), str(obj), include,
                extra=("-mcpu=cortex-m3", "-mthumb"),
            )
            cortex_parts.append(obj)

        lld = find_tool(None, "ld.lld-18", "ld.lld-19", "ld.lld")
        require(lld, "ld.lld is required")
        cortex_linker = root / "freestanding-cortex.ld"
        cortex_linker.write_text(
            "SECTIONS {\n"
            "  . = 0x00000000;\n"
            "  .text : { *(.text*) *(.rodata*) }\n"
            "  .data : { *(.data*) }\n"
            "  .bss : { *(.bss*) *(COMMON) }\n"
            "  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }\n"
            "}\n",
            encoding="utf-8",
        )
        cortex_elf = root / "provider.elf"
        linked = run([
            lld,
            "-m",
            "armelf",
            "-T",
            str(cortex_linker),
            "-e",
            "allocator_probe",
            "-o",
            str(cortex_elf),
            str(cortex_obj),
            *map(str, cortex_parts),
        ])
        require(linked.returncode == 0, linked.stderr)
        undefined = run([llvm_nm, "-u", str(cortex_elf)])
        require(undefined.returncode == 0, undefined.stderr)
        require(not undefined.stdout.strip(),
                "Cortex provider artifact has undefined symbols:\n"
                + undefined.stdout)
        sections = run([llvm_readelf, "-S", str(cortex_elf)])
        require(sections.returncode == 0, sections.stderr)
        require(".ARM.exidx" not in sections.stdout and
                ".ARM.extab" not in sections.stdout,
                "Cortex freestanding artifact retained unwind sections")

        if qemu:
            startup = root / "startup.c"
            startup.write_text(
                "#include <stdint.h>\n"
                "extern int allocator_probe(void);\n"
                "__attribute__((aligned(8))) static uint32_t stack[256];\n"
                "static void semihost_exit(int code) {\n"
                "  struct { uint32_t reason; uint32_t subcode; } block={0x20026u,(uint32_t)code};\n"
                "  register uint32_t op __asm__(\"r0\")=0x20u;\n"
                "  register void* arg __asm__(\"r1\")=&block;\n"
                "  __asm__ volatile(\"bkpt 0xab\" : : \"r\"(op), \"r\"(arg) : \"memory\");\n"
                "  for(;;){}\n"
                "}\n"
                "void Reset_Handler(void){ int r=allocator_probe(); semihost_exit(r==42?0:1); }\n"
                "__attribute__((section(\".isr_vector\"),used))\n"
                "const uintptr_t vectors[2]={(uintptr_t)(stack+256),(uintptr_t)Reset_Handler};\n",
                encoding="utf-8",
            )
            startup_obj = root / "startup.o"
            compile_c(
                clang, "thumbv7m-none-eabi",
                str(startup), str(startup_obj), include,
                extra=("-mcpu=cortex-m3", "-mthumb"),
            )
            linker_script = root / "cortex.ld"
            linker_script.write_text(
                "ENTRY(Reset_Handler)\n"
                "MEMORY {\n"
                "  FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 256K\n"
                "  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K\n"
                "}\n"
                "SECTIONS {\n"
                " .isr_vector : { KEEP(*(.isr_vector)) } > FLASH\n"
                " .text : { *(.text*) *(.rodata*) } > FLASH\n"
                " .data : { *(.data*) } > RAM AT > FLASH\n"
                " .bss : { *(.bss*) *(COMMON) } > RAM\n"
                " /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) }\n"
                "}\n",
                encoding="utf-8",
            )
            qemu_elf = root / "provider-qemu.elf"
            linked = run([
                lld,
                "-m",
                "armelf",
                "-T",
                str(linker_script),
                "-o",
                str(qemu_elf),
                str(startup_obj),
                str(cortex_obj),
                *map(str, cortex_parts),
            ])
            require(linked.returncode == 0, linked.stderr)
            qemu_undefined = run([llvm_nm, "-u", str(qemu_elf)])
            require(qemu_undefined.returncode == 0, qemu_undefined.stderr)
            require(not qemu_undefined.stdout.strip(),
                    "QEMU Cortex artifact has undefined symbols:\n"
                    + qemu_undefined.stdout)
            qemu_sections = run([llvm_readelf, "-S", str(qemu_elf)])
            require(qemu_sections.returncode == 0, qemu_sections.stderr)
            require(".ARM.exidx" not in qemu_sections.stdout and
                    ".ARM.extab" not in qemu_sections.stdout,
                    "QEMU Cortex artifact retained unwind sections")
            emulated = run([
                qemu,
                "-M",
                "lm3s6965evb",
                "-cpu",
                "cortex-m3",
                "-nographic",
                "-semihosting-config",
                "enable=on,target=native",
                "-kernel",
                str(qemu_elf),
            ], timeout=20)
            require(emulated.returncode == 0,
                    "Cortex-M provider QEMU execution failed: "
                    + emulated.stderr[-3000:])

    print(
        "AllocatorProviderV1 freestanding Wasm/Cortex admission, "
        "zero-import/no-undefined artifacts, reset/OOM, and execution gates PASS"
    )


if __name__ == "__main__":
    main()
