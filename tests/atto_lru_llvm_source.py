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
            # A commented-out line or adjacent unrelated CTest is not a
            # registered native guest. Require one exact active invocation.
            matches = [
                line.strip() for line in cmake.splitlines()
                if line.lstrip().startswith(register + " ") and
                line.rstrip().endswith(")")
            ]
            require(len(matches) == 1, "missing/duplicate native CTest: " + name)
            witness = matches[0]
            require((' --run ' + str(expected_exit) + ' --clang "${ORE_CLANG}"') in witness,
                    "wrong native exit/linker oracle: " + name)
            runtime = (' --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/'
                       'src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"')
            require((runtime in witness) == needs_array,
                    "missing or unexpected native array runtime: " + name)
    if check_cmake:
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        require(any(line.lstrip().startswith("add_test(NAME atto_lru_micro_source_inventory")
                    for line in cmake.splitlines()),
                "active source-only integrity CTest missing")

def main() -> None:
    verify(ROOT)
    # Test the oracle itself. All mutations must fail before compilation.
    with tempfile.TemporaryDirectory(prefix="atto-llvm-lru-mutation-") as tmp:
        dst = Path(tmp)
        (dst / PREFIX).mkdir(parents=True)
        shutil.copyfile(ROOT / "CMakeLists.txt", dst / "CMakeLists.txt")
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
        # A dangling symlink must not be accepted as a valid source.
        symlink_case = dst / PREFIX / "probe-wrap.atto"
        symlink_bytes = symlink_case.read_bytes()
        symlink_case.unlink()
        symlink_case.symlink_to("missing.atto")
        try:
            verify(dst, check_cmake=False)
        except RuntimeError as exc:
            require("symlinked LLVM LRU source" in str(exc),
                    "symlink was rejected for wrong reason")
        else:
            raise RuntimeError("symlinked source was admitted")
        symlink_case.unlink()
        symlink_case.write_bytes(symlink_bytes)
        # CTest registration changes must fail even with intact source blobs.
        cmake_path = dst / "CMakeLists.txt"
        original_cmake = cmake_path.read_text(encoding="utf-8")
        marker = "atto_lru_probe_wrap ../../contracts/atto-lru-llvm-v1/probe-wrap.atto --run 17"
        require(marker in original_cmake, "cannot locate pinned test")
        cmake_path.write_text(original_cmake.replace(marker, marker.replace(" --run 17", " --run 0")))
        try:
            verify(dst)
        except RuntimeError as exc:
            require("wrong native exit/linker oracle" in str(exc),
                    "CTest exit mutation rejected for wrong reason")
        else:
            raise RuntimeError("wrong native exit oracle was accepted")
        runtime = ' --native-runtime "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"'
        line = next(line for line in original_cmake.splitlines() if marker in line)
        require(runtime in line, "missing baseline array runtime")
        cmake_path.write_text(original_cmake.replace(line, line.replace(runtime, "")))
        try:
            verify(dst)
        except RuntimeError as exc:
            require("missing or unexpected native array runtime" in str(exc),
                    "missing runtime rejected for wrong reason")
        else:
            raise RuntimeError("missing native array runtime was accepted")
    print("ATTO_LLVM_LRU_SOURCE_ONLY=PASS cases=4 mutations_rejected=7 "
          "native_compiler_guest=NOT_EXECUTED")

if __name__ == "__main__":
    main()
