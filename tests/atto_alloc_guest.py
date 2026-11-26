#!/usr/bin/env python3
"""Real LLVM guest admission for backend-owned Atto alloc source.

Compiles three .atto module graphs to LLVM IR and native executables,
then runs them with original .atto files hidden. Private-owner-token negative
cases must fail before emitting IR. No host-model success is a guest pass.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
MODULES = ("slab", "deque", "actor_heap")


def run(args: list[str], timeout: int = 180) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, capture_output=True, text=True,
                          timeout=timeout, check=False)


def source_file(path: Path) -> str:
    if path.is_symlink() or not path.is_file():
        raise AssertionError(f"missing or symlinked source: {path}")
    raw = path.read_bytes()
    if not raw or len(raw) > 1_048_576 or raw.startswith(b"\xef\xbb\xbf") or b"\0" in raw:
        raise AssertionError(f"invalid source bytes: {path}")
    return raw.decode("utf-8", errors="strict")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()

    runtime_c = ROOT / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    if runtime_c.is_symlink() or not runtime_c.is_file():
        raise AssertionError("missing native allocator runtime C provider")
    native_includes = ROOT / "src/main/cpp"

    compiler = str(Path(args.compiler).resolve(strict=True))
    clang = str(Path(args.clang).resolve(strict=True))
    positive = negative = sanitized = 0
    with tempfile.TemporaryDirectory(prefix="atto-alloc-native-guest-") as work:
        base = Path(work)
        for module in MODULES:
            d = base / module
            d.mkdir()
            (d / (module + ".atto")).write_text(
                source_file(ROOT / "stdlib" / "policy" / "alloc" / (module + ".atto")),
                encoding="utf-8")
            fixture = source_file(ROOT / "tests" / "fixtures" / "alloc" /
                                  (module + "_smoke.atto"))
            original = f'"atto.alloc/{module}"'
            if fixture.count(original) != 1:
                raise AssertionError(f"expected one canonical import in {module}")
            fixture = fixture.replace(original, f'"./{module}"')
            entry = d / "main.atto"
            entry.write_text(fixture, encoding="utf-8")
            result = run([compiler, str(entry)])
            if result.returncode != 0 or not result.stdout.strip():
                raise AssertionError(
                    (module, "source compile failed", result.returncode, result.stderr[:2500]))
            if "define " not in result.stdout or "@main" not in result.stdout:
                raise AssertionError((module, "compiler did not emit native main IR"))
            ll = d / "main.ll"
            ll.write_text(result.stdout, encoding="utf-8")
            native = d / "guest"
            built = run([clang, "-O2", "-Wno-override-module",
                         "-x", "ir", str(ll),
                         "-x", "c", str(runtime_c),
                         "-I", str(native_includes), "-o", str(native)])
            if built.returncode != 0:
                raise AssertionError((module, "link", built.stderr[:2500]))
            # Prove guest has no runtime dependency on .atto source or loader.
            entry.rename(d / "main.atto.hidden")
            (d / (module + ".atto")).rename(d / (module + ".atto.hidden"))
            outcome = run([str(native)], timeout=20)
            if outcome.returncode != 0:
                raise AssertionError((module, "guest", outcome.returncode, outcome.stderr[:2500]))
            positive += 1

            # Sanitizer-linked run is useful evidence, not a proof that every
            # compiler-generated memory access has been instrumented.
            sanitized_exe = d / "guest-sanitized"
            san_link = run([clang, "-O1", "-g",
                            "-fsanitize=address,undefined",
                            "-fno-omit-frame-pointer", "-Wno-override-module",
                            "-x", "ir", str(ll), "-x", "c", str(runtime_c),
                            "-I", str(native_includes), "-o", str(sanitized_exe)])
            if san_link.returncode != 0:
                raise AssertionError((module, "sanitizer link", san_link.stderr[:2500]))
            out = subprocess.run([str(sanitized_exe)], capture_output=True, text=True,
                                 timeout=20, check=False,
                                 env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1:halt_on_error=1",
                                      "UBSAN_OPTIONS": "halt_on_error=1:print_stacktrace=1"})
            if out.returncode != 0:
                raise AssertionError((module, "sanitizer", out.returncode, out.stderr[:2500]))
            sanitized += 1

        # Test exact canonical import spelling against embedded compiler source.
        # These are additional native guests, not a source-only registry lookup.
        for module in MODULES:
            d = base / ("canonical-" + module)
            d.mkdir()
            fixture = source_file(ROOT / "tests" / "fixtures" / "alloc" /
                                  (module + "_smoke.atto"))
            entry = d / "main.atto"
            entry.write_text(fixture, encoding="utf-8")
            emitted = run([compiler, str(entry)])
            if emitted.returncode != 0 or not emitted.stdout.strip():
                raise AssertionError((module, "canonical import", emitted.stderr[:2500]))
            if "define " not in emitted.stdout or "@main" not in emitted.stdout:
                raise AssertionError((module, "canonical compiler emitted no native guest"))
            ll = d / "main.ll"
            ll.write_text(emitted.stdout, encoding="utf-8")
            native = d / "guest"
            linked = run([clang, "-O2", "-Wno-override-module",
                          "-x", "ir", str(ll), "-x", "c", str(runtime_c),
                          "-I", str(native_includes), "-o", str(native)])
            if linked.returncode != 0:
                raise AssertionError((module, "canonical link", linked.stderr[:2500]))
            entry.rename(d / "main.atto.hidden")
            result = run([str(native)], timeout=20)
            if result.returncode != 0:
                raise AssertionError((module, "canonical execution",
                                      result.returncode, result.stderr[:2500]))
            positive += 1

        # Negative checks after positive admission avoid celebrating errors
        # caused merely by unsupported classes, imports or generic syntax.
        attacks = {
            "slab": '''import module slab from "./slab";
pub fnc main(): int {
  let mut slab.Slab s = take! slab.new_slab(17, 1);
  let slab.Handle h = s.insert(5).unwrap();
  let slab.OwnerToken stolen = h.owner_token;
  return 0;
}''',
            "actor_heap": '''import module actor_heap from "./actor_heap";
pub fnc main(): int {
  let mut actor_heap.ActorHeap h = take! actor_heap.new_actor_heap(19, 8, 3);
  let actor_heap.HeapHandle a = h.allocate(1).unwrap();
  let actor_heap.OwnerToken stolen = a.owner_token;
  return 0;
}''',
        }
        for module, source in attacks.items():
            d = base / ("negative-" + module)
            d.mkdir()
            (d / (module + ".atto")).write_text(
                source_file(ROOT / "stdlib" / "policy" / "alloc" / (module + ".atto")),
                encoding="utf-8")
            entry = d / "main.atto"
            entry.write_text(source, encoding="utf-8")
            rejected = run([compiler, str(entry)])
            if not 0 < rejected.returncode < 128 or rejected.stdout.strip():
                raise AssertionError((module, "private field access admitted or IR leaked",
                                      rejected.returncode, rejected.stderr[:1500]))
            negative += 1
    if positive != 6 or sanitized != 3 or negative != 2:
        raise AssertionError("incomplete guest and negative fixture coverage")
    print(f"ATTO_ALLOC_LLVM_COMPILED_GUEST_PASS positive={positive} "
          f"sanitizer_linked={sanitized} private_denied={negative}")


if __name__ == "__main__":
    main()
