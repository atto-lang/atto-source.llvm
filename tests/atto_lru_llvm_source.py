#!/usr/bin/env python3
"""Source-only LLVM LRU microtrace inventory; NEVER a guest execution receipt."""
from __future__ import annotations

import hashlib
import shutil
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = Path("contracts/atto-lru-llvm-v1")
CASES = {
    "hash-quotient": ("83cbc0e7c1bad761a9127ab0fcbacf70efc68de1", 42, False),
    "probe-wrap": ("a2ac8c42146684f3b0bcf5cde26dd111ff6867ae", 17, True),
    "slot-recycle": ("342f2691520613ebc6edd37d55816c3c8e6b0343", 29, True),
    "recency-touch": ("91051df9cda94d6a94beaff7cde44f3b8da13ea0", 61, True),
}

def require(ok: bool, msg: str) -> None:
    if not ok:
        raise RuntimeError(msg)

def blob(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()

def verify(root: Path, check_cmake: bool = True) -> None:
    for name, (expected_blob, expected_exit, needs_array) in CASES.items():
        relative = PREFIX / (name + ".atto")
        current = root
        for component in relative.parts:
            current = current / component
            require(not current.is_symlink(), "symlinked LLVM LRU source: " + name)
        require(current.is_file(), "missing LLVM LRU source: " + name)
        raw = current.read_bytes()
        require(0 < len(raw) < 8192 and blob(raw) == expected_blob,
                "unreviewed LLVM LRU source: " + name)
        text = raw.decode("utf-8")
        executable = "\n".join(
            line.split("//", 1)[0] for line in text.splitlines()
        )
        require("%" not in executable and "^" not in executable,
                "removed AttoLang operator: " + name)
        require("pub routine main()" in executable or "pub fnc main()" in executable,
                "missing native entrypoint: " + name)
        require(("Array<int>" in executable) == needs_array,
                "wrong primitive-array surface: " + name)
        if check_cmake:
            cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
            path = "../../" + relative.as_posix()
            register = "oreslang_contract_test(atto_lru_" + name.replace("-", "_") + " " + path
            require(register in cmake, "unregistered LLVM guest: " + name)
            require("--run " + str(expected_exit) in cmake[cmake.index(register):cmake.index(register)+260],
                    "wrong native exit oracle: " + name)
    if check_cmake:
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        require("atto_lru_micro_source_inventory" in cmake,
                "source-only integrity CTest missing")

def main() -> None:
    verify(ROOT)
    # Test the oracle itself. All mutations must fail before compilation.
    with tempfile.TemporaryDirectory(prefix="atto-llvm-lru-mutation-") as tmp:
        dst = Path(tmp)
        (dst / PREFIX).mkdir(parents=True)
        for name in CASES:
            path = PREFIX / (name + ".atto")
            shutil.copyfile(ROOT / path, dst / path)
        for name in CASES:
            path = dst / PREFIX / (name + ".atto")
            original = path.read_bytes()
            path.write_bytes(original + b"\n// injected mutation\n")
            try:
                verify(dst, check_cmake=False)
            except RuntimeError as exc:
                require("unreviewed LLVM LRU source" in str(exc),
                        "mutation rejected for wrong reason: " + name)
            else:
                raise RuntimeError("LLVM source mutation was accepted: " + name)
            path.write_bytes(original)
    print("ATTO_LLVM_LRU_SOURCE_ONLY=PASS cases=4 mutations_rejected=4 "
          "native_compiler_guest=NOT_EXECUTED")

if __name__ == "__main__":
    main()
