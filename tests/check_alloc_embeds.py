#!/usr/bin/env python3
"""Ensure embedded LLVM compiler library strings exactly match their .atto files.

This is source identity, NOT native guest execution evidence.
"""
from pathlib import Path
import hashlib

ROOT = Path(__file__).resolve().parents[1]
ITEMS = (("slab", "Slab", "ATTO_SLAB"), ("deque", "Deque", "ATTO_DEQUE"),
         ("actor_heap", "ActorHeap", "ATTO_AHEAP"))

def check() -> None:
    for module, cls, delim in ITEMS:
        source = ROOT / "stdlib" / "alloc" / (module + ".atto")
        generated = ROOT / "src/main/cpp/oreslang/frontend" / ("Generated" + cls + "Atto.inc")
        if source.is_symlink() or generated.is_symlink() or not source.is_file() or not generated.is_file():
            raise AssertionError("source/embedded file must be regular and not symlinked")
        raw = source.read_bytes()
        if not raw or len(raw) > 1_048_576 or bytes([0]) in raw:
            raise AssertionError("invalid source bytes")
        value = raw.decode("utf-8", errors="strict")
        if ")" + delim + '"' in value:
            raise AssertionError("unsafe raw string delimiter")
        digest = hashlib.sha1(b"blob " + str(len(raw)).encode() + bytes([0]) + raw).hexdigest()
        expected = ("// Generated from stdlib/alloc/" + module + ".atto; do not edit manually.\n"
                    + "// source Git blob SHA-1: " + digest + "\n"
                    + 'R"' + delim + "(" + value + ")" + delim + '"\n')
        if generated.read_bytes() != expected.encode("utf-8"):
            raise AssertionError("compiler embedded source drift for " + module)
        print("ATTO_ALLOC_EMBED_IDENTITY module=" + module + " blob=" + digest)
    print("ATTO_ALLOC_EMBED_SOURCE_ONLY_PASS=3;GUEST=NOT_EXECUTED")

if __name__ == "__main__":
    check()
