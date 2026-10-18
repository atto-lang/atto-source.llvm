#!/usr/bin/env python3
"""Freestanding C ABI-shaped actor firmware ELF/QEMU proof."""
import argparse, pathlib, shutil, struct, subprocess, tempfile

def demand(ok, message):
    if not ok: raise RuntimeError(message)

def cmd(parts, timeout=60, check=True):
    args=[str(p) for p in parts]
    proc=subprocess.run(args,timeout=timeout,capture_output=True,text=True)
    text=proc.stdout+proc.stderr
    if check and proc.returncode:
        raise RuntimeError("command failed: %s: %s" % (" ".join(args), text))
    return proc.returncode,text

def validate_elf(path, readelf):
    raw=path.read_bytes()
    demand(len(raw)>=52 and raw[:6]==b"\x7fELF\x01\x01","not little ELF32")
    demand(struct.unpack_from("<HH",raw,16)==(2,40),"not ARM ET_EXEC")
    entry,phoff=struct.unpack_from("<II",raw,24)
    phlen,count=struct.unpack_from("<HH",raw,42)
    demand(phlen>=32 and count>0 and phoff+count*phlen<=len(raw),"bad ELF program-header bounds")
    spans,physical,executable,vector=[],[],[],None
    for i in range(count):
        tp,ofs,va,pa,filesz,memsz,flags,align=struct.unpack_from("<IIIIIIII",raw,phoff+i*phlen)
        if tp!=1: continue
        demand(filesz<=memsz and ofs+filesz<=len(raw),"bad ELF segment")
        demand(not ((flags&3)==3),"writable+executable segment")
        if va<0x40000: demand(va+memsz<=0x40000,"out-of-range flash")
        else: demand(0x20000000<=va<0x20010000 and va+memsz<=0x20010000,"out-of-range SRAM")
        for start,end in spans: demand(not (va<end and start<va+memsz),"overlapping RAM/flash loads")
        spans.append((va,va+memsz))
        if filesz:
            demand(pa<0x40000 and pa+filesz<=0x40000,"file-backed image not in flash")
            for start,end in physical: demand(not (pa<end and start<pa+filesz),"overlapping physical loads")
            physical.append((pa,pa+filesz))
        if flags&1: executable.append((va,va+filesz))
        if va==0 and filesz>=8:
            demand(vector is None,"duplicate vectors")
            vector=struct.unpack_from("<II",raw,ofs)
    demand(vector is not None,"no flash vectors")
    sp,reset=vector
    demand(sp==0x20010000 and reset==entry and (reset&1),"bad reset/initial stack")
    demand(any(start<=(reset&~1)<end for start,end in executable),"reset outside executable region")
    _,symbols=cmd([readelf,"-Ws",path])
    undefined=[line for line in symbols.splitlines() if len(line.split())>=8 and line.split()[6]=="UND" and line.split()[7]]
    demand(not undefined,"undefined ARM symbols: "+repr(undefined))
    _,dyn=cmd([readelf,"-d",path])
    demand("NEEDED" not in dyn,"dynamic dependency")
    print("ABI_ACTOR_ELF_VALID",path.name,"entry",hex(entry))

if __name__=="__main__":
    raise SystemExit("helper module; use freestanding_source_actor_compiler_qemu.py")
