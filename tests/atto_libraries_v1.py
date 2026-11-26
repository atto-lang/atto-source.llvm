#!/usr/bin/env python3
"""Compiler-owned core/alloc/std library guests: compile, link, run natively.

Each tests/atto_libraries/*.atto file declares, in leading comments:
  // flags: <extra compiler flags>
  // expect-exit: N            (compile, link with the native runtime, run)
  // expect-stdout-json: "..." (exact stdout, JSON string)
  // expect-reject             (compiler must reject the source)
  // expect-error: <substring> (rejection diagnostic must contain this)
  // expect-error-llvm: <substring> (LLVM-specific diagnostic)
The corpus is shared with the other atto-source.* compilers.
"""
import argparse
import json
import pathlib
import shlex
import subprocess
import sys
import tempfile

from atto_library_case_guard import validate_corpus, validate_case, rejection_valid

RUNTIME = ["NativeRecordRegion.c", "NativeStringRegion.c", "NativeIntArrayRegion.c"]


def headers(path):
    meta = {"flags": [], "errors": []}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line.startswith("//"):
            break
        body = line[2:].strip()
        if body == "expect-reject":
            meta["reject"] = True
        for key in ("flags", "expect-exit", "expect-stdout-json", "expect-error", "expect-error-llvm"):
            if body.startswith(key + ":"):
                value = body[len(key) + 1:].strip()
                if key == "flags":
                    meta["flags"] = shlex.split(value)
                elif key == "expect-exit":
                    meta["exit"] = int(value)
                elif key == "expect-stdout-json":
                    meta["stdout"] = json.loads(value)
                else:
                    meta["reject"] = True
                    meta["errors"].append(value)
    return meta


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    args.compiler = str(pathlib.Path(args.compiler).resolve())
    root = pathlib.Path(__file__).resolve().parents[1]
    runtime_dir = root / "src/main/cpp/oreslang/runtime"
    include = root / "src/main/cpp"
    cases = sorted((root / "tests/atto_libraries").glob("*.atto"))
    validate_corpus(cases)
    failures = []
    with tempfile.TemporaryDirectory(prefix="atto-libs-") as tmp:
        tmp = pathlib.Path(tmp)
        objects = []
        for name in RUNTIME:
            obj = tmp / (name + ".o")
            subprocess.run([args.clang, "-O1", "-std=c11", "-I", str(include), "-c",
                            str(runtime_dir / name), "-o", str(obj)], check=True)
            objects.append(str(obj))
        for case in cases:
            meta = headers(case)
            validate_case(case.name, meta)
            compiled = subprocess.run([args.compiler, *meta["flags"], case.name],
                                      cwd=case.parent, capture_output=True, text=True, timeout=120)
            if meta.get("reject"):
                missing = [e for e in meta["errors"] if e not in compiled.stderr]
                if not rejection_valid(compiled, meta['errors']):
                    failures.append(f"{case.name}: expected rejection containing "
                                    f"{missing!r}, got rc={compiled.returncode} {compiled.stderr.strip()[:300]}")
                else:
                    print(f"PASS {case.name} (rejected)")
                continue
            if compiled.returncode != 0:
                failures.append(f"{case.name}: compile failed: {compiled.stderr.strip()[:400]}")
                continue
            ir = tmp / (case.stem + ".ll")
            ir.write_text(compiled.stdout, encoding="utf-8")
            exe = tmp / case.stem
            obj = tmp / (case.stem + ".o")
            subprocess.run([args.clang, "-Wno-override-module", "-x", "ir", str(ir), "-c", "-o", str(obj)], check=True)
            subprocess.run([args.clang, str(obj), *objects, "-o", str(exe)], check=True)
            ran = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
            out = ran.stdout.decode("utf-8", "strict")
            if ran.returncode != meta.get("exit", 0):
                failures.append(f"{case.name}: exit {ran.returncode} != {meta.get('exit', 0)}")
            elif "stdout" in meta and out != meta["stdout"]:
                failures.append(f"{case.name}: stdout {out!r} != {meta['stdout']!r}")
            else:
                print(f"PASS {case.name} (exit {ran.returncode})")
    for failure in failures:
        print("FAIL " + failure)
    print(f"ATTO_LIBRARIES_V1 {len(cases) - len(failures)}/{len(cases)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
