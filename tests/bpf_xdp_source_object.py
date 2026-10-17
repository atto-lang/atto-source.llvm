#!/usr/bin/env python3
"""Experimental eBPF XDP: fail-closed .ores source to real BPF ELF object."""
import argparse
import pathlib
import re
import shutil
import subprocess
import tempfile


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=90)


def require(ok, message):
    if not ok:
        raise RuntimeError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--compiler", required=True)
    args = p.parse_args()
    clang = shutil.which("clang")
    require(bool(clang), "Clang with BPF backend required for eBPF object test")
    readelf = shutil.which("readelf")
    require(bool(readelf), "readelf required to inspect eBPF ELF sections")
    with tempfile.TemporaryDirectory(prefix="ores-bpf-") as tmp:
        root = pathlib.Path(tmp)
        src = root / "probe.ores"
        cmd = [args.compiler, "--target=bpfel", str(src)]

        def reject(code, fragment="ebpf"):
            src.write_text(code, encoding="utf-8")
            proc = run(cmd)
            require(proc.returncode != 0 and proc.stdout == "",
                    "unsafe eBPF source admitted: " + repr(code[:140]))
            require(fragment in proc.stderr.lower(),
                    "unexpected eBPF rejection diagnostic: " + proc.stderr)

        for action in range(5):
            src.write_text("pub fnc ores_xdp(): int { return %d; }\n" % action,
                           encoding="utf-8")
            result = run(cmd)
            require(result.returncode == 0, result.stderr)
            ir = result.stdout
            require('target triple = "bpfel"' in ir, "wrong eBPF target triple")
            require('section "xdp"' in ir and 'section "license"' in ir,
                    "missing XDP or license section")
            require(re.search(r"ret i32 %d(?:\\s|$)" % action, ir), "wrong XDP return code")
            require("define i32 @ores_xdp(" in ir, "missing XDP entrypoint")
            require("define i32 @main" not in ir, "host entrypoint leaked into BPF")

            irfile = root / "program.ll"
            objfile = root / "program.o"
            irfile.write_text(ir, encoding="utf-8")
            compiled = run([clang, "--target=bpfel", "-O2", "-x", "ir", "-c",
                            str(irfile), "-o", str(objfile)])
            require(compiled.returncode == 0, "real BPF backend unavailable: " + compiled.stderr)
            elf = objfile.read_bytes()
            require(len(elf) >= 64 and elf[:4] == b"\\x7fELF" and elf[4:6] == b"\\x02\\x01",
                    "BPF result not ELF64 little-endian")
            require(int.from_bytes(elf[18:20], "little") == 247,
                    "wrong ELF e_machine (expected EM_BPF=247)")
            sections = run([readelf, "-W", "-S", str(objfile)])
            require(sections.returncode == 0 and
                    re.search(r"\\s+xdp\\s+", sections.stdout) and
                    re.search(r"\\s+license\\s+", sections.stdout),
                    "missing real ELF XDP/license sections")

        reject("pub fnc main(): int { return 2; }")
        reject("fnc ores_xdp(): int { return 2; }")
        reject("pub fnc ores_xdp(int ctx): int { return 2; }")
        reject("pub fnc ores_xdp(): int { return 2 + 0; }")
        reject("pub fnc ores_xdp(): int { return 5; }")
        reject("pub fnc ores_xdp(): int { return -1; }")
        reject("pub fnc ores_xdp(): int { return 2; }\nfnc helper(): int { return 1; }")
        reject("pub fnc ores_xdp(): int { val int v = 2; return v; }")
        reject("pub fnc ores_xdp(): int { return ores_xdp(); }")
        reject("pub fnc ores_xdp(): int { return 2; }\n" + "// extra\n" * 17000,
               "source exceeds")
        src.write_text("pub fnc ores_xdp(): int { return 2; }\n", encoding="utf-8")
        for flags in (["--target=bpfel", "--target=bpfel"],
                      ["--target=bpfel", "--experimental-owned-int"],
                      ["--target=bpfeb"]):
            proc = run([args.compiler, *flags, str(src)])
            require(proc.returncode != 0 and not proc.stdout,
                    "invalid BPF CLI option accepted: " + repr(flags))
    print("eBPF XDP object/triple/section/actions and fail-closed admission PASS; kernel verifier not tested")


if __name__ == "__main__":
    main()
