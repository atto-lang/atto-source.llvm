#!/usr/bin/env python3
"""Real Oreslang source -> BPF IR -> ELF64/EM_BPF smoke, NOT kernel verifier evidence."""
import argparse
import pathlib
import shutil
import struct
import subprocess
import tempfile

def run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                          text=True, check=False, timeout=45)

def require(condition, message):
    if not condition:
        raise RuntimeError(message)

def elf_sections(blob):
    require(blob[:4] == b"\x7fELF" and blob[4:6] == b"\x02\x01",
            "expected little-endian ELF64")
    require(len(blob) >= 64 and blob[6] == 1 and
            struct.unpack_from("<I", blob, 20)[0] == 1,
            "invalid ELF format version or incomplete header")
    require(struct.unpack_from("<H", blob, 52)[0] == 64 and
            struct.unpack_from("<H", blob, 56)[0] == 0,
            "invalid ELF64 header size or unsupported program headers")
    require(struct.unpack_from("<H", blob, 16)[0] == 1,
            "expected relocatable ET_REL, not an arbitrary linked image")
    require(struct.unpack_from("<H", blob, 18)[0] == 247,
            "expected EM_BPF=247, not a host or Wasm object")
    offset = struct.unpack_from("<Q", blob, 40)[0]
    entry_size = struct.unpack_from("<H", blob, 58)[0]
    count = struct.unpack_from("<H", blob, 60)[0]
    names_idx = struct.unpack_from("<H", blob, 62)[0]
    require(0 < count < 256 and entry_size == 64 and names_idx < count,
            "invalid ELF section table geometry")
    require(offset + entry_size * count <= len(blob), "section table beyond ELF")
    def entry(index):
        base = offset + index * entry_size
        name = struct.unpack_from("<I", blob, base)[0]
        data_offset, size = struct.unpack_from("<QQ", blob, base + 24)
        section_type = struct.unpack_from("<I", blob, base + 4)[0]
        # SHT_NOBITS has a logical size but no stored section bytes.
        if section_type == 8:
            require(data_offset <= len(blob), "NOBITS offset outside ELF")
        else:
            require(data_offset + size <= len(blob), "section outside ELF")
        section_flags = struct.unpack_from("<Q", blob, base + 8)[0]
        return name, data_offset, size, section_type, section_flags
    # No executable or metadata bytes may hide inside ELF headers,
    # another section's payload or the section-header table itself.
    # An SHT_NOBITS section reserves memory, not file bytes.
    occupied = [(0, 64), (offset, offset + entry_size * count)]
    for index in range(count):
        _, data_offset, size, kind, _ = entry(index)
        if kind == 8 or size == 0:
            continue
        start, end = data_offset, data_offset + size
        require(start >= 64, "ELF section overlaps file header")
        require(all(end <= lo or start >= hi for lo, hi in occupied),
                "ELF section data overlaps another section or header table")
        occupied.append((start, end))
    _, strings_offset, strings_size, names_kind, _ = entry(names_idx)
    require(names_kind == 3 and strings_size > 1,
            "ELF section names table must be a nonempty STRTAB")
    strings = blob[strings_offset:strings_offset + strings_size]
    found = {}
    for index in range(count):
        name_index, data_offset, size, section_type, section_flags = entry(index)
        require(name_index < len(strings), "name offset outside string table")
        end = strings.find(b"\0", name_index)
        require(end >= 0, "unterminated section name")
        name = strings[name_index:end].decode("ascii")
        require(name not in found, "duplicate ELF section name")
        found[name] = (data_offset, size, section_type, section_flags)
    require("xdp" in found and "license" in found, "missing xdp or license ELF section")
    # Other executable sections must be empty. No hidden kernel programs or
    # silently linked host/runtime code may ride along in this first slice.
    for name, (_, length, _, flags) in found.items():
        if name != "xdp" and (flags & 4) and length:
            raise RuntimeError("unexpected nonempty executable ELF section: " + name)
    code_start, code_size, code_type, code_flags = found["xdp"]
    require(code_type == 1 and (code_flags & 4) == 4 and
            8 <= code_size <= 32768 and code_size % 8 == 0,
            "xdp section is not a bounded executable BPF instruction stream")
    program = blob[code_start:code_start + code_size]
    opcodes = program[::8]
    require(opcodes[-1] == 0x95, "xdp section must end in BPF_EXIT")
    # No runtime subprograms or unapproved privileged helpers are allowed in
    # this first source-level policy slice. BPF_CALL is opcode 0x85.
    require(0x85 not in opcodes, "XDP guest contains unapproved BPF_CALL")
    license_offset, license_size, license_type, license_flags = found["license"]
    require(license_type == 1 and not (license_flags & 4) and license_size == 4,
            "invalid executable or missing license section")
    require(blob[license_offset:license_offset + license_size] == b"MIT\0",
            "test-only BPF license bytes differ from reviewed prototype metadata")
    return found

def verify_packet_dependent_machine_code(raw, sections):
    # A syntactically correct XDP/ELF could still be an optimized
    # constant-return stub, silently ignoring guest packet policy.
    # Inspect the real BPF instruction bytes rather than only LLVM IR.
    start, length, _, _ = sections["xdp"]
    instructions = [raw[start + k:start + k + 8] for k in range(0, length, 8)]
    # LDX W (0x61) at ctx+0 and ctx+4, or an LLVM-combined
    # LDX DW (0x79) at ctx+0. A successful Clang codegen must preserve
    # both XDP context inputs to the checked nonempty predicate.
    word_offsets = {struct.unpack_from("<h", insn, 2)[0]
                    for insn in instructions if insn[0] == 0x61}
    double_offsets = {struct.unpack_from("<h", insn, 2)[0]
                      for insn in instructions if insn[0] == 0x79}
    require({0, 4}.issubset(word_offsets) or 0 in double_offsets,
            "XDP object does not load both ctx.data and ctx.data_end")
    # At least one conditional jump is required to distinguish the
    # empty/nonempty packet test, not simply a constant action.
    conditional_jumps = {
        0x15, 0x1d, 0x25, 0x2d, 0x35, 0x3d, 0x45, 0x4d,
        0x55, 0x5d, 0x65, 0x6d, 0x75, 0x7d, 0xa5, 0xad,
        0xb5, 0xbd, 0xc5, 0xcd, 0xd5, 0xdd,
        0x16, 0x1e, 0x26, 0x2e, 0x36, 0x3e, 0x46, 0x4e,
        0x56, 0x5e, 0x66, 0x6e, 0x76, 0x7e, 0xa6, 0xae,
        0xb6, 0xbe, 0xc6, 0xce, 0xd6, 0xde,
    }
    require(any(insn[0] in conditional_jumps for insn in instructions),
            "XDP policy compiled to a constant/unconditional action")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    clang = shutil.which("clang")
    require(bool(clang), "Clang BPF backend required")
    with tempfile.TemporaryDirectory(prefix="oreslang-bpf-xdp-") as dirname:
        root = pathlib.Path(dirname)
        source = root / "xdp.ores"
        ir = root / "xdp.ll"
        obj = root / "xdp.o"
        cmd = (args.compiler, "--deployment-profile=ebpf-xdp",
               "--experimental-ebpf-xdp-ir", str(source))
        source.write_text("""pub fnc xdp_action(int packet_nonempty): int {
    if (packet_nonempty == 0) { return 1; } fi
    return 2;
}
""")
        generated = run(cmd)
        require(generated.returncode == 0, "XDP source rejection: " + generated.stderr)
        require('target triple = "bpfel"' in generated.stdout, "missing BPF triple")
        require("define i32 @oreslang_xdp(" in generated.stdout,
                "missing real XDP entry ABI")
        require('section "xdp"' in generated.stdout and 'section "license"' in generated.stdout,
                "missing BPF sections")
        require("@oreslang_xdp_policy" in generated.stdout,
                "source policy was not lowered")
        ir.write_text(generated.stdout)
        built = run((clang, "--target=bpfel", "-O2", "-x", "ir",
                     "-c", str(ir), "-o", str(obj)))
        require(built.returncode == 0 and obj.exists(),
                "Clang BPF object emission failure: " + built.stderr)
        raw = obj.read_bytes()
        sections = elf_sections(raw)
        require({"xdp", "license"}.issubset(sections),
                "BPF object lacks xdp/license section: " + repr(sections))
        verify_packet_dependent_machine_code(raw, sections)
        # Prove the ELF parser itself rejects forged host objects and helper
        # calls; otherwise a broken test could give false-green confidence.
        for offset, replacement in [(18, bytes((0x3e, 0))), (16, bytes((2, 0)))]:
            corrupted = bytearray(raw)
            corrupted[offset:offset + 2] = replacement
            try:
                elf_sections(corrupted)
            except RuntimeError:
                pass
            else:
                raise RuntimeError("ELF object mutation was not rejected")
        code_start, _, _, _ = sections["xdp"]
        corrupted = bytearray(raw)
        corrupted[code_start] = 0x85  # BPF_CALL without approval
        try:
            elf_sections(corrupted)
        except RuntimeError:
            pass
        else:
            raise RuntimeError("forged helper call was not rejected")

        cases = [
            ("fnc xdp_action(int packet_nonempty): int { return 2; }\n", "requires"),
            ("pub fnc main(int packet_nonempty): int { return 2; }\n", "xdp"),
            ("pub fnc xdp_action(): int { return 2; }\n", "xdp"),
            ("pub fnc xdp_action(int packet_nonempty): int { return xdp_action(packet_nonempty); }\n", "xdp"),
            ("pub fnc xdp_action(int packet_nonempty): int { return unknown; }\n", None),
            ("pub fnc xdp_action(int packet_nonempty): int { return 2; }\nfnc second(): int { return 2; }\n", "xdp"),
            ("pub fnc xdp_action(int packet_nonempty): int { val x: int = 2; return x; }\n", "xdp"),
            ("pub fnc xdp_action(int packet_nonempty): int { return 3 / 2; }\n", None),
            ("pub fnc xdp_action(int packet_nonempty): int { return 0; }\n", "outcome"),
            ("pub fnc xdp_action(int packet_nonempty): int { return 3; }\n", "outcome"),
            ("pub fnc xdp_action(int packet_nonempty): int { return 4; }\n", "outcome"),
            ("pub fnc xdp_action(int packet_nonempty): int { return packet_nonempty; }\n", "outcome"),
        ]
        for code, needle in cases:
            source.write_text(code)
            result = run(cmd)
            require(result.returncode != 0 and not result.stdout,
                    "eBPF rejected case emitted IR: " + code)
            if needle is not None:
                require(needle in result.stderr.lower(), "unexpected diagnostic: " + result.stderr)
        source.write_text("pub fnc xdp_action(int packet_nonempty): int { return 2; }\n")
        for flags in [
            ("--target=host",), ("--target=wasm32",),
            ("--deployment-profile=ebpf-tc",),
            ("--experimental-owned-int",),
            ("--experimental-rt-scalar-loop",),
        ]:
            result = run((args.compiler, *flags, "--deployment-profile=ebpf-xdp",
                          "--experimental-ebpf-xdp-ir", str(source)))
            require(result.returncode != 0 and not result.stdout,
                    "mixed target/experiment accepted: " + repr(flags))
    print("PASS: actual .ores XDP policy -> BPF LLVM IR -> EM_BPF ELF with xdp/license sections; negatives pass")
    print("NOT VERIFIED: loading into a target kernel, BTF/CO-RE or physical packet handling")

if __name__ == "__main__":
    main()
