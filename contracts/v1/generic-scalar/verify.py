#!/usr/bin/env python3
"""Deterministic cross-backend source-level generic scalar contract runner.

LLVM native_exit cases compile to real IR, link with Clang and execute.
Graal admission mode checks source typing only; it is NOT runtime equivalence.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent
FIXTURES = (ROOT / "fixtures").resolve()

def load_suite():
    manifest = ROOT / "cases.json"
    blob = manifest.read_bytes()
    doc = json.loads(blob)
    if doc.get("schema_version") != "1.0.0" or doc.get("scope") != "free-function-scalar-generic-v1":
        raise ValueError("unknown generic scalar contract version or scope")
    ids, paths = set(), set()
    h = hashlib.sha256(b"oreslang-generic-scalar-v1\0")
    for key, data in [("cases.json", blob)]:
        raw = key.encode("utf-8")
        h.update(len(raw).to_bytes(4, "big") + raw + len(data).to_bytes(8, "big") + data)
    for case in doc.get("cases", []):
        name = case.get("id")
        rel = case.get("source")
        if not isinstance(name, str) or not name.startswith("generic.scalar.") or name in ids:
            raise ValueError(f"invalid or duplicate case id: {name}")
        if not isinstance(rel, str) or not rel.startswith("fixtures/") or not rel.endswith(".ores"):
            raise ValueError(f"invalid source reference: {rel}")
        source = (ROOT / rel).resolve()
        if source.parent != FIXTURES or source.is_symlink() or not source.is_file():
            raise ValueError(f"fixture outside trusted directory: {rel}")
        if rel in paths:
            raise ValueError(f"fixture reused: {rel}")
        contents = source.read_bytes()
        if not contents or len(contents) > 8192:
            raise ValueError(f"empty or oversized fixture: {rel}")
        if case.get("kind") not in ("native_exit", "diagnostic"):
            raise ValueError(f"invalid case kind: {name}")
        if case["kind"] == "native_exit":
            if type(case.get("expected_exit")) is not int or not 0 <= case["expected_exit"] <= 125 or "diagnostics" in case:
                raise ValueError(f"invalid expected exit: {name}")
        elif "expected_exit" in case:
            raise ValueError(f"diagnostic case has exit expectation: {name}")
        statuses = case.get("backends")
        if not isinstance(statuses, dict) or set(statuses) != {"llvm", "graal"}:
            raise ValueError(f"backend inventory mismatch: {name}")
        if any(s not in ("required", "pending") for s in statuses.values()):
            raise ValueError(f"unsupported backend status: {name}")
        pending = "pending" in statuses.values()
        if pending != bool(case.get("pending_reason")):
            raise ValueError(f"pending reason mismatch: {name}")
        if case["kind"] == "diagnostic":
            diag = case.get("diagnostics")
            if not isinstance(diag, dict):
                raise ValueError(f"missing diagnostic map: {name}")
            for backend, status in statuses.items():
                if status == "required" and (not isinstance(diag.get(backend), str) or not diag[backend]):
                    raise ValueError(f"required diagnostic missing: {name}/{backend}")
        raw = rel.encode("utf-8")
        h.update(len(raw).to_bytes(4, "big") + raw + len(contents).to_bytes(8, "big") + contents)
        ids.add(name)
        paths.add(rel)
    if len(ids) < 10:
        raise ValueError("generic scalar suite unexpectedly reduced")
    found = {"fixtures/" + p.name for p in FIXTURES.glob("*.ores")}
    if found != paths:
        raise ValueError(f"orphaned or missing fixtures: {sorted(found ^ paths)}")
    return doc["cases"], h.hexdigest()

def execute(command):
    return subprocess.run(command, capture_output=True, timeout=70, check=False)

def validate_backend(cases, backend, compiler, clang):
    passed, pending = 0, 0
    with tempfile.TemporaryDirectory(prefix="oreslang-generic-corpus-") as tmp:
        temp = Path(tmp)
        for case in cases:
            if case["backends"][backend] == "pending":
                pending += 1
                print(f"PENDING {backend}: {case['id']} ({case['pending_reason']})", flush=True)
                continue
            source = (ROOT / case["source"]).resolve()
            if backend == "llvm":
                compile_result = execute([str(compiler), str(source)])
            else:
                compile_result = execute([str(compiler), "--mode=aot", "--check", str(source)])
            if case["kind"] == "diagnostic":
                marker = case["diagnostics"][backend].encode()
                # Crash / signal / exec failure cannot satisfy an expected diagnostic.
                if compile_result.returncode <= 0 or marker not in (compile_result.stderr + compile_result.stdout):
                    raise AssertionError(f"{case['id']}: wrong diagnostic; exit {compile_result.returncode}, stderr {compile_result.stderr!r}")
                if backend == "llvm" and compile_result.stdout:
                    raise AssertionError(f"{case['id']}: rejected source still emitted LLVM IR")
            else:
                if compile_result.returncode != 0:
                    raise AssertionError(f"{case['id']}: compile/check failed: {compile_result.stderr!r}")
                if backend == "llvm":
                    if not compile_result.stdout.startswith(b"; ModuleID"):
                        raise AssertionError(f"{case['id']}: no valid native compiler IR")
                    ll = temp / (case["id"].replace(".", "-") + ".ll")
                    exe = temp / (case["id"].replace(".", "-") + ".guest")
                    ll.write_bytes(compile_result.stdout)
                    linked = execute([str(clang), "-Wno-override-module", "-x", "ir", str(ll), "-o", str(exe)])
                    if linked.returncode != 0:
                        raise AssertionError(f"{case['id']}: Clang failed: {linked.stderr!r}")
                    result = execute([str(exe)])
                    if result.returncode != case["expected_exit"]:
                        raise AssertionError(f"{case['id']}: guest exit {result.returncode} != {case['expected_exit']}")
            passed += 1
            print(f"PASS {backend}: {case['id']}", flush=True)
    print(f"BACKEND {backend}: {passed} passed, {pending} pending", flush=True)
    if backend == "graal":
        print("GRAAL_ADMISSION_ONLY: executable exit-code parity NOT certified", flush=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--validate-only", action="store_true")
    ap.add_argument("--backend", choices=("llvm", "graal"))
    ap.add_argument("--compiler", type=Path)
    ap.add_argument("--clang", type=Path)
    args = ap.parse_args()
    cases, digest = load_suite()
    print(f"GENERIC_SCALAR_SUITE_SHA256={digest}; cases={len(cases)}", flush=True)
    if args.validate_only:
        return
    if not args.backend or not args.compiler:
        ap.error("--backend and --compiler required unless --validate-only")
    if args.backend == "llvm" and not args.clang:
        ap.error("--clang required for LLVM guest execution")
    validate_backend(cases, args.backend, args.compiler.resolve(), args.clang.resolve() if args.clang else None)

if __name__ == "__main__":
    main()
