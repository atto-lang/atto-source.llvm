#!/usr/bin/env python3
"""Standalone hostile-ELF selftest for eBPF object admission (no LLVM needed)."""
import struct

from experimental_ebpf_xdp_source_object import elf_sections


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
    print("PASS: standalone BPF ELF parser, fourteen hostile-machine/code/section probes")


if __name__ == "__main__":
    main()
