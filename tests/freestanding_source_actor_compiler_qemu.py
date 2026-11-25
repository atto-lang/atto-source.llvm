#!/usr/bin/env python3
import argparse
import pathlib
import re
import shutil
import subprocess
import tempfile
from freestanding_source_actor_qemu import cmd, demand, validate_elf

EXPECTED = (
    "oreslang_cortexm_bootstrap",
    "oreslang_actor_Source_init", "oreslang_actor_Source_on_message",
    "oreslang_actor_Sink_init", "oreslang_actor_Sink_on_message",
    "oreslang_source_actor_spawn", "oreslang_source_actor_send",
    "oreslang_source_actor_stop", "ores_actor_i64_heap_field_init",
    "ores_actor_i64_heap_field_init_mutable", "ores_actor_i64_heap_field_read",
    "ores_actor_i64_heap_field_write",
)

def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    p.add_argument("--clang", default=shutil.which("clang"))
    p.add_argument("--readelf", default=shutil.which("readelf"))
    p.add_argument("--qemu", default=shutil.which("qemu-system-arm"))
    p.add_argument("--link-only", action="store_true")
    a = p.parse_args()
    demand(a.clang and a.readelf and (a.link_only or a.qemu),
           "clang/readelf/QEMU required; no soft skip allowed")
    root = pathlib.Path(__file__).resolve().parent.parent
    guest = root / "tests/freestanding_source_actors_compiler.ores"
    runtime = root / "src/main/cpp/oreslang/runtime"
    includedir = root / "src/main/cpp"
    with tempfile.TemporaryDirectory(prefix="ores-compiler-actors-") as tmp:
        work = pathlib.Path(tmp); irfile = work / "source-actors.ll"
        response = subprocess.run([a.compiler, "--target=thumbv7m-none-eabi", str(guest)],
                                  capture_output=True, text=True, timeout=90)
        demand(response.returncode == 0 and response.stdout.strip(),
               "real Oreslang compiler failed: " + response.stderr)
        ir = response.stdout
        missing = [symbol for symbol in EXPECTED if "@"+symbol not in ir]
        demand(not missing, "compiler-generated actor ABI missing: "+repr(missing))
        forbidden = ("@pthread_", "@calloc(", "@malloc(", "@free(", "@abort(")
        demand(not any(symbol in ir for symbol in forbidden),
               "hosted runtime imported by compiler-emitted guest")
        for actor in ("Source", "Sink"):
            handler = "oreslang_actor_"+actor+"_on_message"
            demand(re.search(r"define\s+i64\s+@"+handler+r"\(", ir) is not None,
                   "guest actor handler definition absent: "+handler)
        irfile.write_text(ir)
        print("ACTUAL_ORESLANG_ACTOR_IR_VERIFIED", len(ir.splitlines()), "lines")
        flags = [a.clang, "--target=thumbv7m-none-eabi", "-mcpu=cortex-m3",
                 "-mthumb", "-Oz", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-fno-unwind-tables",
                 "-fno-asynchronous-unwind-tables", "-I", includedir]
        cmd(flags + ["-c", runtime/"FreestandingActorKernel.c", "-o", work/"kernel.o"])
        cmd(flags + ["-c", runtime/"FreestandingSourceActor.c", "-o", work/"adapter.o"])
        cmd(flags + ["-c", root/"tests/freestanding_source_actor_startup.S",
                     "-o", work/"startup.o"])
        cmd(flags + ["-x", "ir", "-c", irfile, "-o", work/"actual-oreslang-actors.o"])
        for negative in (False, True):
            name = "negative" if negative else "positive"
            c_obj = work/(name+"-harness.o")
            cmd(flags + (["-DNEGATIVE_GUEST"] if negative else []) +
                ["-c", root/"tests/freestanding_source_actor_compiler_firmware.c",
                 "-o", c_obj])
            elf = work/(name+"-source.elf")
            cmd([a.clang, "--target=thumbv7m-none-eabi", "-fuse-ld=lld",
                 "-nostdlib", "-nodefaultlibs", "-Wl,--no-undefined",
                 "-Wl,-T,"+str(root/"tests/freestanding_source_actor_linker.ld"),
                 work/"kernel.o", work/"adapter.o", work/"startup.o",
                 work/"actual-oreslang-actors.o", c_obj, "-o", elf])
            validate_elf(elf, a.readelf)
            if a.link_only: continue
            try:
                code, output = cmd([a.qemu, "-M", "lm3s6965evb", "-cpu", "cortex-m3",
                     "-nographic", "-monitor", "none", "-serial", "none",
                     "-semihosting-config", "enable=on,target=native",
                     "-kernel", elf], timeout=20, check=False)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("source actor QEMU timeout: "+name) from exc
            passed, failed = "ORES_FREESTANDING_ABI_PASS", "ORES_FREESTANDING_ABI_FAIL"
            if negative:
                demand(code != 0 and failed in output and passed not in output,
                       "negative source actor firmware escaped failure path: "+repr((code, output)))
            else:
                demand(code == 0 and passed in output and failed not in output,
                       "source-defined actors failed under QEMU: "+repr((code, output)))
            print("SOURCE_DEFINED_ORESLANG_ACTORS_QEMU_"+name.upper()+"_PASS", code)
    print("SOURCE_DEFINED_ORESLANG_ACTORS_"+("LINK" if a.link_only else "QEMU")+"_PASS")
if __name__ == "__main__": main()
