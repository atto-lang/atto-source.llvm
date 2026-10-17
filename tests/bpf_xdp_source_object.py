#!/usr/bin/env python3
"""Experimental eBPF XDP: fail-closed .ores source to real BPF ELF object."""
import argparse
import hashlib
import os
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
    p.add_argument("--output-dir", type=pathlib.Path)
    args = p.parse_args()
    clang = shutil.which("clang")
    require(bool(clang), "Clang with BPF backend required for eBPF object test")
    readelf = shutil.which("readelf")
    require(bool(readelf), "readelf required to inspect eBPF ELF sections")
    if args.output_dir:
        args.output_dir.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ores-bpf-") as tmp:
        root = pathlib.Path(tmp)
        src = root / "probe.ores"
        cmd = [args.compiler, "--target=bpfel", "--bpf-program=xdp", str(src)]

        def reject_bytes(payload, fragment=None):
            src.write_bytes(payload)
            proc = run(cmd)
            require(proc.returncode != 0 and proc.stdout == "",
                    "unsafe eBPF source admitted: " + repr(payload[:140]))
            if fragment is not None:
                require(fragment in proc.stderr.lower(),
                        "unexpected eBPF rejection diagnostic: " + proc.stderr)
            return proc

        def reject(code, fragment="ebpf"):
            return reject_bytes(code.encode("utf-8"), fragment)

        observed_ir = {}
        observed_obj = {}
        for action in range(4):
            source_text = "pub fnc ores_xdp(): int { return %d; }\n" % action
            src.write_text(source_text, encoding="utf-8")
            result = run(cmd)
            require(result.returncode == 0, result.stderr)
            ir = result.stdout
            second = run(cmd)
            require(second.returncode == 0 and second.stdout == ir,
                    "same source did not produce deterministic LLVM IR")
            observed_ir[action] = ir
            require('target triple = "bpfel"' in ir, "wrong eBPF target triple")
            require('target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"' in ir,
                    "wrong eBPF data layout")
            require('section "xdp"' in ir and 'section "license"' in ir,
                    "missing XDP or license section")
            require(re.search(r"ret i32 %d(?:\s|$)" % action, ir), "wrong XDP return code")
            require("define i32 @ores_xdp(" in ir, "missing XDP entrypoint")
            require("define i32 @main" not in ir, "host entrypoint leaked into BPF")

            irfile = root / "program.ll"
            objfile = root / "program.o"
            irfile.write_text(ir, encoding="utf-8")
            compiled = run([clang, "--target=bpfel", "-O2", "-x", "ir", "-c",
                            str(irfile), "-o", str(objfile)])
            require(compiled.returncode == 0, "real BPF backend unavailable: " + compiled.stderr)
            elf = objfile.read_bytes()
            objfile2 = root / "program-second.o"
            compiled2 = run([clang, "--target=bpfel", "-O2", "-x", "ir", "-c",
                             str(irfile), "-o", str(objfile2)])
            require(compiled2.returncode == 0, compiled2.stderr)
            elf2 = objfile2.read_bytes()
            require(elf2 == elf,
                    "same IR did not produce deterministic BPF object: %s != %s" %
                    (hashlib.sha256(elf).hexdigest(), hashlib.sha256(elf2).hexdigest()))
            observed_obj[action] = elf

            # Stage-0 validity must not depend on optimizer cleanup. Produce an
            # unoptimized object from the exact same verified IR as well.
            objfile_o0 = root / "program-o0.o"
            compiled_o0 = run([clang, "--target=bpfel", "-O0", "-x", "ir", "-c",
                               str(irfile), "-o", str(objfile_o0)])
            require(compiled_o0.returncode == 0,
                    "Clang -O0 BPF backend failed: " + compiled_o0.stderr)
            elf_o0 = objfile_o0.read_bytes()
            require(len(elf_o0) >= 64 and elf_o0[:6] == b"\x7fELF\x02\x01" and
                    int.from_bytes(elf_o0[18:20], "little") == 247,
                    "-O0 result is not little-endian EM_BPF ELF")
            sections_o0 = run([readelf, "-W", "-S", str(objfile_o0)])
            require(sections_o0.returncode == 0, sections_o0.stderr)
            require(re.search(r"\s+xdp\s+", sections_o0.stdout) and
                    re.search(r"\s+license\s+", sections_o0.stdout),
                    "-O0 EM_BPF ELF missing xdp/license sections")
            require(".eh_frame" not in sections_o0.stdout and
                    ".maps" not in sections_o0.stdout and
                    ".BTF" not in sections_o0.stdout,
                    "-O0 stage-0 object contains unwind/maps/BTF metadata")
            reloc_o0 = run([readelf, "-W", "-r", str(objfile_o0)])
            require(reloc_o0.returncode == 0 and
                    "There are no relocations in this file." in reloc_o0.stdout,
                    "-O0 stage-0 BPF object unexpectedly requires relocations")
            require(len(elf) >= 64 and elf[:4] == b"\x7fELF" and elf[4:6] == b"\x02\x01",
                    "BPF result not ELF64 little-endian")
            require(int.from_bytes(elf[16:18], "little") == 1,
                    "BPF result is not relocatable ET_REL")
            require(int.from_bytes(elf[18:20], "little") == 247,
                    "wrong ELF e_machine (expected EM_BPF=247)")
            require(int.from_bytes(elf[20:24], "little") == 1,
                    "wrong ELF version")
            sections = run([readelf, "-W", "-S", str(objfile)])
            require(sections.returncode == 0 and
                    re.search(r"\s+xdp\s+", sections.stdout) and
                    re.search(r"\s+license\s+", sections.stdout),
                    "missing real ELF XDP/license sections")
            require(".maps" not in sections.stdout and ".BTF" not in sections.stdout and
                    ".BTF.ext" not in sections.stdout and ".eh_frame" not in sections.stdout,
                    "stage-0 object unexpectedly contains maps/BTF/unwind metadata")
            require(len(re.findall(r"\s+xdp\s+", sections.stdout)) == 1,
                    "stage-0 object must contain exactly one xdp section")
            require(len(re.findall(r"\s+license\s+", sections.stdout)) == 1,
                    "stage-0 object must contain exactly one license section")
            symbols = run([readelf, "-W", "-s", str(objfile)])
            require(symbols.returncode == 0, symbols.stderr)
            undefined = []
            named_globals = []
            for line in symbols.stdout.splitlines():
                cols = line.split()
                if len(cols) >= 8 and cols[6] == "UND" and cols[7]:
                    undefined.append(cols[7])
                if len(cols) >= 8 and cols[4] == "GLOBAL" and cols[7]:
                    named_globals.append(cols[7])
            require(not undefined, "stage-0 BPF object has undefined symbols: " + repr(undefined))
            require("ores_xdp" in named_globals,
                    "stage-0 object missing global ores_xdp symbol")
            require(all(name in ("ores_xdp", "_license") for name in named_globals),
                    "unexpected global BPF symbols: " + repr(named_globals))
            reloc = run([readelf, "-W", "-r", str(objfile)])
            require(reloc.returncode == 0 and "There are no relocations in this file." in reloc.stdout,
                    "stage-0 BPF object unexpectedly requires relocations")
            license_dump = run([readelf, "-x", "license", str(objfile)])
            compact = re.sub(r"[^0-9a-f]", "", license_dump.stdout.lower())
            require(license_dump.returncode == 0 and "50726f707269657461727900" in compact,
                    "stage-0 license marker is not Proprietary\\0")
            objdump = shutil.which("llvm-objdump")
            if objdump:
                dis = run([objdump, "-d", str(objfile)])
                require(dis.returncode == 0, dis.stderr)
                lower = dis.stdout.lower()
                require("call" not in lower,
                        "stage-0 BPF object unexpectedly contains helper/subprogram call")
                require("exit" in lower,
                        "stage-0 BPF object missing exit instruction")
            if args.output_dir:
                shutil.copyfile(objfile, args.output_dir / ("xdp-%d.o" % action))
                shutil.copyfile(objfile, args.output_dir / ("xdp-%d-O2.o" % action))
                shutil.copyfile(objfile_o0, args.output_dir / ("xdp-%d-O0.o" % action))

        # Verifier-aware packet byte slice. The source sees only an opaque,
        # borrowed XdpContext; the compiler owns data/data_end extraction and
        # inserts the bounds branch before the packet load.
        packet_source = (
            "pub fnc ores_xdp(borrow XdpContext ctx): int {\n"
            "  if (ctx.packet_u8(12) == 8) { return 1; } fi\n"
            "  return 2;\n"
            "}\n"
        )
        src.write_text(packet_source, encoding="utf-8")
        packet_result = run(cmd)
        require(packet_result.returncode == 0, packet_result.stderr)
        packet_ir = packet_result.stdout
        require("packet.load.0" in packet_ir and "packet.next.0" in packet_ir,
                "packet_u8 lowering lacks explicit checked CFG")
        require("icmp ule" in packet_ir and "load i8" in packet_ir,
                "packet_u8 lowering lacks bounds compare or byte load")
        require("ret i32 1" in packet_ir and "ret i32 2" in packet_ir,
                "packet_u8 lowering lost guarded/fallback actions")
        packet_ll = root / "packet-u8.ll"
        packet_obj = root / "packet-u8.o"
        packet_ll.write_text(packet_ir, encoding="utf-8")
        packet_compiled = run([clang, "--target=bpfel", "-O2", "-x", "ir", "-c",
                               str(packet_ll), "-o", str(packet_obj)])
        require(packet_compiled.returncode == 0,
                "packet_u8 BPF object failed: " + packet_compiled.stderr)
        packet_sections = run([readelf, "-W", "-S", str(packet_obj)])
        require(packet_sections.returncode == 0 and
                re.search(r"\s+xdp\s+", packet_sections.stdout) and
                ".maps" not in packet_sections.stdout and ".BTF" not in packet_sections.stdout,
                "packet_u8 object violated stage-1 section boundary")
        packet_symbols = run([readelf, "-W", "-s", str(packet_obj)])
        require(packet_symbols.returncode == 0 and " UND " not in packet_symbols.stdout,
                "packet_u8 object has unresolved symbols")
        packet_reloc = run([readelf, "-W", "-r", str(packet_obj)])
        require(packet_reloc.returncode == 0 and
                "There are no relocations in this file." in packet_reloc.stdout,
                "packet_u8 object unexpectedly requires relocations")
        if args.output_dir:
            shutil.copyfile(packet_obj, args.output_dir / "xdp-packet-u8.o")

        # Harmless lexical/parenthesization differences must normalize to the
        # exact same verified IR as the canonical XDP_PASS source.
        for equivalent in (
            "pub fnc ores_xdp(): int { return (2); }\n",
            "pub fnc ores_xdp(): int { return (((2))); }\n",
            "// leading comment\npub fnc ores_xdp(): int {\n  return 2;\n}\n",
            "pub   fnc   ores_xdp ( ) : int { return 2 ; }\n",
        ):
            src.write_text(equivalent, encoding="utf-8")
            normalized = run(cmd)
            require(normalized.returncode == 0, normalized.stderr)
            require(normalized.stdout == observed_ir[2],
                    "equivalent XDP_PASS source did not normalize to identical IR")

        # Admission boundary: exactly 4096 bytes is permitted, 4097 is rejected
        # before parsing. Padding is a trailing source comment.
        base = b"pub fnc ores_xdp(): int { return 2; }\n"
        # Every incomplete prefix before the final closing brace/newline form
        # must fail without leaking a partial module. The complete source minus
        # only its trailing newline is intentionally excluded because it is valid.
        for cut in range(len(base) - 1):
            reject_bytes(base[:cut], None)

        exact = base + b"//" + (b"x" * (4096 - len(base) - 3)) + b"\n"
        require(len(exact) == 4096, "test bug: exact BPF boundary is not 4096 bytes")
        src.write_bytes(exact)
        boundary = run(cmd)
        require(boundary.returncode == 0 and 'target triple = "bpfel"' in boundary.stdout,
                "exact 4096-byte BPF source was not admitted: " + boundary.stderr)
        reject_bytes(exact + b"x", "source exceeds")

        # Native source reader/UTF-8 validation must fail before the parser and
        # must never leak partial LLVM IR.
        reject_bytes(base[:-1] + b"\x00\n", "nul byte")
        reject_bytes(base + b"//\xc0\xaf\n", "valid utf-8")
        reject_bytes(base + b"//\xed\xa0\x80\n", "valid utf-8")
        reject_bytes(base + b"//\xf4\x90\x80\x80\n", "valid utf-8")
        reject_bytes(base + b"//\xe2\x82", "valid utf-8")

        # Descriptor-level source admission: symlinks to regular files are
        # allowed, but special files/directories are rejected without reading
        # from a potentially blocking stream. Limits still apply after symlink.
        regular = root / "regular.ores"
        regular.write_bytes(base)
        linked = root / "linked.ores"
        os.symlink(regular.name, linked)
        linked_run = run([args.compiler, "--target=bpfel", "--bpf-program=xdp", str(linked)])
        require(linked_run.returncode == 0 and linked_run.stdout == observed_ir[2],
                "regular source symlink did not preserve exact BPF semantics")

        too_big = root / "too-big.ores"
        too_big.write_bytes(exact + b"x")
        too_big_link = root / "too-big-link.ores"
        os.symlink(too_big.name, too_big_link)
        linked_big = run([args.compiler, "--target=bpfel", "--bpf-program=xdp",
                          str(too_big_link)])
        require(linked_big.returncode != 0 and not linked_big.stdout and
                "source exceeds" in linked_big.stderr.lower(),
                "BPF byte limit was bypassed through source symlink")

        directory_run = run([args.compiler, "--target=bpfel", "--bpf-program=xdp",
                             str(root)])
        require(directory_run.returncode != 0 and not directory_run.stdout and
                "regular file" in directory_run.stderr.lower(),
                "directory source was not rejected")

        fifo = root / "source.fifo"
        os.mkfifo(fifo)
        fifo_run = run([args.compiler, "--target=bpfel", "--bpf-program=xdp",
                        str(fifo)])
        require(fifo_run.returncode != 0 and not fifo_run.stdout and
                "regular file" in fifo_run.stderr.lower(),
                "FIFO source was not rejected fail-closed/nonblocking")

        reject("pub fnc main(): int { return 2; }")
        reject("fnc ores_xdp(): int { return 2; }")
        reject("pub fnc ores_xdp(int ctx): int { return 2; }")
        reject("pub fnc ores_xdp(): int { return 2 + 0; }")
        reject("pub fnc ores_xdp(): int { return 4; }")
        reject("pub fnc ores_xdp(): int { return 5; }")
        reject("pub fnc ores_xdp(): int { return -1; }")
        reject("pub fnc ores_xdp(): int { return 2; }\nfnc helper(): int { return 1; }")
        reject("pub fnc ores_xdp(): int { val v: int = 2; return v; }")
        reject("pub fnc ores_xdp(): int { return ores_xdp(); }")
        reject("pub fnc ores_xdp<T>(): int { return 2; }", None)
        reject("pub fnc ores_xdp(): void { return; }", None)
        reject("pub fnc ores_xdp(): int { if (true) { return 2; } return 1; }", None)
        reject("pub fnc ores_xdp(XdpContext ctx): int { return 2; }")
        reject("pub fnc ores_xdp(borrow mut XdpContext ctx): int { return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx, int x): int { return 2; }")
        reject("pub fnc ores_xdp(borrow Foo ctx): int { return 2; }")
        reject("pub fnc ores_xdp(): int { if (ctx.packet_u8(12) == 8) { return 1; } fi return 2; }", None)
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if (ctx.packet_u8(4096) == 8) { return 1; } fi return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if (ctx.packet_u8(-1) == 8) { return 1; } fi return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if (ctx.packet_u8(12) == 256) { return 1; } fi return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if (ctx.packet_u8(12) == 8) { return 4; } fi return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if (ctx.packet_u16(12) == 8) { return 1; } fi return 2; }")
        reject("pub fnc ores_xdp(borrow XdpContext ctx): int { if ((ctx.packet_u8(12) + 0) == 8) { return 1; } fi return 2; }")
        reject("// ores:target=bpfel\npub fnc ores_xdp(): int { return 2; }",
               "environment headers")
        reject("pub fnc ores_xdp(): int { return 2; }\n" + ("// extra\n" * 2000),
               "source exceeds")
        src.write_text("pub fnc ores_xdp(): int { return 2; }\n", encoding="utf-8")
        for good_flags in (["--target=bpfel", "--bpf-program=xdp"],
                           ["--bpf-program=xdp", "--target=bpfel"],
                           ["--no-core-log", "--target=bpfel", "--bpf-program=xdp"],
                           ["--target=bpfel", "--core-log-level=off", "--bpf-program=xdp"]):
            good = run([args.compiler, *good_flags, str(src)])
            require(good.returncode == 0 and 'target triple = "bpfel"' in good.stdout,
                    "valid BPF CLI ordering failed: " + repr(good_flags) + " " + good.stderr)

        cli_cases = (
            (["--target=bpfel", "--target=bpfel", "--bpf-program=xdp"], 2),
            (["--target=bpfel", "--bpf-program=xdp", "--bpf-program=xdp"], 2),
            (["--target=bpfel", "--experimental-owned-int", "--bpf-program=xdp"], 1),
            (["--target=bpfel"], 2),
            (["--bpf-program=xdp"], 2),
            (["--target=bpfel", "--bpf-program=socket"], 2),
            (["--target=bpfel", "--bpf-program="], 2),
            (["--target=bpfeb", "--bpf-program=xdp"], 2),
            (["--target=bpfel", "--bpf-program=xdp", "--cpu=v3"], 2),
        )
        for flags, expected_code in cli_cases:
            proc = run([args.compiler, *flags, str(src)])
            require(proc.returncode == expected_code and not proc.stdout,
                    "wrong fail-closed CLI result for %r: code=%d stderr=%r" %
                    (flags, proc.returncode, proc.stderr))
    print("eBPF XDP admission/bounds/UTF8/determinism/ELF/symbol/relocation/action gates PASS; kernel verifier separate")


if __name__ == "__main__":
    main()
