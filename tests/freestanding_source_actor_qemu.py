#!/usr/bin/env python3
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile

def demand(ok, message):
    if not ok:
        raise RuntimeError(message)

def cmd(parts, timeout=60, check=True):
    args = [str(p) for p in parts]
    proc = subprocess.run(args, timeout=timeout, capture_output=True, text=True)
    text = proc.stdout + proc.stderr
    if check and proc.returncode:
        raise RuntimeError("command failed: %s: %s" % (" ".join(args), text))
    return proc.returncode, text

def validate_elf(path, readelf):
    raw = path.read_bytes()
    demand(len(raw) >= 52 and raw[:6] == b"\x7fELF\x01\x01", "not little ELF32")
    demand(struct.unpack_from("<HH", raw, 16) == (2,40), "not ARM ET_EXEC")
    entry, phoff = struct.unpack_from("<II", raw, 24)
    phlen, count = struct.unpack_from("<HH", raw, 42)
    demand(phlen >= 32 and count > 0 and phoff+count*phlen <= len(raw),
           "bad ELF program-header bounds")
    spans, physical, executable, vector = [], [], [], None
    for i in range(count):
        tp,ofs,va,pa,filesz,memsz,flags,align = struct.unpack_from(
            "<IIIIIIII", raw, phoff+i*phlen)
        if tp != 1: continue
        demand(filesz <= memsz and ofs+filesz <= len(raw), "bad ELF segment")
        demand(not ((flags&3)==3), "writable+executable segment")
        if va < 0x40000:
            demand(va+memsz <= 0x40000, "out-of-range flash")
        else:
            demand(0x20000000 <= va < 0x20010000 and va+memsz <= 0x20010000,
                   "out-of-range SRAM")
        for start,end in spans:
            demand(not (va < end and start < va+memsz), "overlapping RAM/flash loads")
        spans.append((va,va+memsz))
        if filesz:
            demand(pa < 0x40000 and pa+filesz <= 0x40000,
                   "file-backed image not in flash")
            for start,end in physical:
                demand(not (pa<end and start<pa+filesz), "overlapping physical loads")
            physical.append((pa,pa+filesz))
        if flags&1: executable.append((va,va+filesz))
        if va==0 and filesz>=8:
            demand(vector is None, "duplicate vectors")
            vector=struct.unpack_from("<II",raw,ofs)
    demand(vector is not None, "no flash vectors")
    sp,reset=vector
    demand(sp==0x20010000 and reset==entry and (reset&1), "bad reset/initial stack")
    demand(any(start <= (reset&~1) < end for start,end in executable),
           "reset outside executable region")
    _,symbols=cmd([readelf,"-Ws",path])
    undefined=[line for line in symbols.splitlines()
               if len(line.split())>=8 and line.split()[6]=="UND" and line.split()[7]]
    demand(not undefined,"undefined ARM symbols: "+repr(undefined))
    _,dyn=cmd([readelf,"-d",path])
    demand("NEEDED" not in dyn,"dynamic dependency")
    print("ABI_ACTOR_ELF_VALID",path.name,"entry",hex(entry))

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--clang", default=shutil.which("clang"))
    p.add_argument("--readelf", default=shutil.which("readelf"))
    p.add_argument("--qemu", default=shutil.which("qemu-system-arm"))
    p.add_argument("--link-only", action="store_true")
    a=p.parse_args()
    demand(a.clang and a.readelf and (a.qemu or a.link_only),"missing compiler, readelf or QEMU")
    root=pathlib.Path(__file__).resolve().parent.parent
    include=root/"src/main/cpp"; runtime=include/"oreslang/runtime"
    with tempfile.TemporaryDirectory(prefix="ores-source-actor-") as tmp:
        out=pathlib.Path(tmp)
        common=[a.clang,"--target=thumbv7m-none-eabi","-mcpu=cortex-m3","-mthumb",
                "-Oz","-std=c11","-ffreestanding","-fno-builtin","-fno-stack-protector",
                "-fno-unwind-tables","-fno-asynchronous-unwind-tables","-I",include]
        for path,name in ((runtime/"FreestandingActorKernel.c","kernel"),
                          (runtime/"FreestandingSourceActor.c","adapter"),
                          (root/"tests/freestanding_source_actor_startup.S","startup")):
            cmd(common+["-c",path,"-o",out/(name+".o")])
        for neg in (False,True):
            name="negative" if neg else "positive"; fw=out/(name+".o")
            cmd(common+(["-DNEGATIVE_GUEST"] if neg else [])+
                ["-c",root/"tests/freestanding_source_actor_firmware.c","-o",fw])
            elf=out/(name+".elf")
            cmd([a.clang,"--target=thumbv7m-none-eabi","-fuse-ld=lld",
                 "-nostdlib","-nodefaultlibs","-Wl,--no-undefined",
                 "-Wl,-T,"+str(root/"tests/freestanding_source_actor_linker.ld"),
                 out/"kernel.o",out/"adapter.o",out/"startup.o",fw,"-o",elf])
            validate_elf(elf,a.readelf)
            if a.link_only: continue
            try:
                code,stdout=cmd([a.qemu,"-M","lm3s6965evb","-cpu","cortex-m3",
                                 "-nographic","-monitor","none","-serial","none",
                                 "-semihosting-config","enable=on,target=native","-kernel",elf],
                                timeout=20,check=False)
            except subprocess.TimeoutExpired as exc:
                raise RuntimeError("QEMU firmware hung: "+name) from exc
            if neg:
                demand(code != 0 and "ORES_FREESTANDING_ABI_FAIL" in stdout and
                       "ORES_FREESTANDING_ABI_PASS" not in stdout,
                       "negative guest unexpectedly passed: "+repr((code,stdout)))
            else:
                demand(code == 0 and "ORES_FREESTANDING_ABI_PASS" in stdout and
                       "ORES_FREESTANDING_ABI_FAIL" not in stdout,
                       "positive guest did not pass: "+repr((code,stdout)))
            print("ABI_ACTOR_QEMU",name,"exit",code,"PASS")
    print("ABI_ACTOR_FIRMWARE_LINK_PASS" if a.link_only else "ABI_ACTOR_QEMU_PASS")
if __name__=="__main__": main()
