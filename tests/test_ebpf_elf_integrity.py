#!/usr/bin/env python3
"""Standalone hostile-ELF selftest for eBPF object admission (no LLVM needed)."""
import struct

from experimental_ebpf_xdp_source_object import elf_sections, verify_packet_dependent_machine_code


def fixture():
    raw = bytearray(512)
    raw[:7] = b"\x7fELF\x02\x01\x01"
    struct.pack_into("<H", raw, 16, 1)  # ET_REL
    struct.pack_into("<I", raw, 20, 1)  # ELF current version
    struct.pack_into("<H", raw, 52, 64) # ELF64 header size
    struct.pack_into("<H", raw, 18, 247)  # EM_BPF
    struct.pack_into("<Q", raw, 40, 256)  # section header table offset
    struct.pack_into("<H", raw, 58, 64)  # section header entry size
    struct.pack_into("<H", raw, 60, 4)   # section count
    struct.pack_into("<H", raw, 62, 1)   # shstrtab index
    names = b"\0.shstrtab\0xdp\0license\0"
    raw[0x60:0x60 + len(names)] = names
    raw[0x80:0x88] = bytes((0x95, 0, 0, 0, 0, 0, 0, 0))  # BPF_EXIT
    raw[0x90:0x94] = b"MIT\0"

    def section(idx, name, kind, flags, offset, length):
        struct.pack_into("<IIQQQQIIQQ", raw, 256 + 64 * idx,
                         names.index(name), kind, flags, 0,
                         offset, length, 0, 0, 1, 0)
    section(1, b".shstrtab", 3, 0, 0x60, len(names))
    section(2, b"xdp", 1, 6, 0x80, 8)
    section(3, b"license", 1, 2, 0x90, 4)
    return raw


def must_reject(mutator):
    obj = fixture()
    mutator(obj)
    try:
        elf_sections(obj)
    except (RuntimeError, ValueError, UnicodeDecodeError, struct.error):
        return
    raise AssertionError("hostile ELF modification was not denied")


def policy_fixture():
    obj = fixture()
    # Construct an artificial program, not an actual compiler output:
    # two LDX-W context loads, a conditional JNE, and EXIT. This only
    # unit-tests machine-code admission predicates.
    code = bytearray(32)
    code[0] = 0x61
    code[8] = 0x61
    struct.pack_into("<h", code, 8 + 2, 4)
    code[16] = 0x55
    code[24] = 0x95
    obj[0xa0:0xc0] = code
    obj[0xd0:0xd4] = b"MIT\\0"
    struct.pack_into("<Q", obj, 384 + 24, 0xa0)
    struct.pack_into("<Q", obj, 384 + 32, len(code))
    struct.pack_into("<Q", obj, 448 + 24, 0xd0)
    return obj


def must_reject_bad_policy(mutator):
    obj = policy_fixture()
    mutator(obj)
    sections = elf_sections(obj)
    try:
        verify_packet_dependent_machine_code(obj, sections)
    except RuntimeError:
        return
    raise AssertionError("packet-independent BPF stub passed policy machine checks")


def main():
    obj = fixture()
    sections = elf_sections(obj)
    assert "xdp" in sections and "license" in sections
    must_reject(lambda b: struct.pack_into("<H", b, 18, 62))       # x86_64
    must_reject(lambda b: struct.pack_into("<H", b, 16, 2))        # executable vs ET_REL
    must_reject(lambda b: b.__setitem__(6, 2))                    # invalid ELF version
    must_reject(lambda b: struct.pack_into("<I", b, 20, 2))       # bad ELF header version
    must_reject(lambda b: struct.pack_into("<H", b, 52, 52))      # bad ELF64 header size
    must_reject(lambda b: struct.pack_into("<H", b, 56, 1))       # extra program headers
    must_reject(lambda b: struct.pack_into("<H", b, 62, 99))       # bad shstrndx
    must_reject(lambda b: struct.pack_into("<Q", b, 40, 0xffff))   # bad table offset
    must_reject(lambda b: struct.pack_into("<Q", b, 384 + 8, 2))  # xdp non-exec
    must_reject(lambda b: struct.pack_into("<Q", b, 384 + 32, 9)) # unaligned BPF
    must_reject(lambda b: b.__setitem__(0x80, 0x85))               # unapproved BPF_CALL
    must_reject(lambda b: b.__setitem__(0x80, 0x00))               # no BPF_EXIT
    must_reject(lambda b: struct.pack_into("<I", b, 448, 11))    # duplicate xdp section
    must_reject(lambda b: struct.pack_into("<Q", b, 448 + 8, 4)) # executable license
    must_reject(lambda b: struct.pack_into("<Q", b, 328, 4))     # hidden executable string section
    must_reject(lambda b: b.__setitem__(0x90, 0))                # false license bytes
    valid_policy = policy_fixture()
    verify_packet_dependent_machine_code(valid_policy, elf_sections(valid_policy))
    must_reject_bad_policy(lambda b: b.__setitem__(0xa0 + 8, 0))  # missing data_end load
    must_reject_bad_policy(lambda b: b.__setitem__(0xa0 + 16, 0)) # missing branch
    print("PASS: synthetic ELF parser 16 corruptions and policy machine-code 2 negative probes")


if __name__ == "__main__":
    main()
