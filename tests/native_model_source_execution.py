#!/usr/bin/env python3
"""Shared-corpus native define-model source -> LLVM IR -> Clang execution gate.

The normative cases originate in common-source executable-v1-conformance.json
and are vendored as the deterministic line-oriented projection supplied by
--corpus. This proves only model-core-i64-v1 on the selected native host.
"""
import argparse
from dataclasses import dataclass
from pathlib import Path
from typing import Optional
import subprocess
import tempfile

_MAX_CAPTURE = 2 * 1024 * 1024
_EXPECTED_REVISION = "model-core-i64-v1.2026-10-20.10"
_EXPECTED_PROFILE = "model-core-i64-v1"


@dataclass(frozen=True)
class Case:
    case_id: str
    expect: str
    java_mode: str
    llvm_mode: str
    diagnostic: Optional[str]
    llvm_diagnostic: Optional[str]
    graph_digest: Optional[str]
    configured_digest: Optional[str]
    semantic_result: Optional[int]
    configuration_i64: Optional[int]
    source: str


def nullable(value: str) -> Optional[str]:
    return None if value == "-" else value


def decode_ascii_hex(value: str) -> str:
    raw = bytes.fromhex(value)
    if any(byte > 0x7F for byte in raw):
        raise AssertionError("shared model corpus source is not ASCII")
    return raw.decode("ascii")


def load_corpus(path: Path) -> list[Case]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "ORESLANG_MODEL_EXECUTABLE_CASES_V1":
        raise AssertionError("invalid shared model corpus header")
    if len(lines) < 3:
        raise AssertionError("truncated shared model corpus")
    if lines[1] != f"revision|{_EXPECTED_REVISION}":
        raise AssertionError(f"unexpected corpus revision: {lines[1]!r}")
    if lines[2] != f"profile|{_EXPECTED_PROFILE}":
        raise AssertionError(f"unexpected corpus profile: {lines[2]!r}")

    cases: list[Case] = []
    for line in lines[3:]:
        if not line or line.startswith("identity|"):
            continue
        fields = line.split("|")
        if len(fields) != 12 or fields[0] != "case":
            raise AssertionError(f"malformed shared case record: {line[:120]!r}")
        cases.append(Case(
            case_id=fields[1],
            expect=fields[2],
            java_mode=fields[3],
            llvm_mode=fields[4],
            diagnostic=nullable(fields[5]),
            llvm_diagnostic=nullable(fields[6]),
            graph_digest=nullable(fields[7]),
            configured_digest=nullable(fields[8]),
            semantic_result=None if fields[9] == "-" else int(fields[9]),
            configuration_i64=None if fields[10] == "-" else int(fields[10]),
            source=decode_ascii_hex(fields[11]),
        ))
    if len(cases) < 20:
        raise AssertionError(f"shared model corpus unexpectedly small: {len(cases)}")
    return cases


def run(argv):
    with tempfile.TemporaryFile() as out, tempfile.TemporaryFile() as err:
        proc = subprocess.run(argv, stdout=out, stderr=err, timeout=65, check=False)
        out.seek(0, 2)
        err.seek(0, 2)
        if out.tell() > _MAX_CAPTURE or err.tell() > _MAX_CAPTURE:
            raise AssertionError("model compiler/tool emitted oversized output")
        out.seek(0)
        err.seek(0)
        return proc.returncode, out.read(), err.read()


def compile_source(root: Path, compiler: Path, item: Case):
    src = root / f"{item.case_id}.ores"
    src.write_text(item.source, encoding="utf-8")
    rc, out, err = run([str(compiler), str(src)])
    return src, rc, out, err


def verify(compiler: Path, clang: Path, corpus_path: Path):
    cases = load_corpus(corpus_path)
    with tempfile.TemporaryDirectory(prefix="oreslang-model-guest-") as td:
        root = Path(td)
        for item in cases:
            _, rc, out, err = compile_source(root, compiler, item)

            if item.llvm_mode == "reject":
                diagnostic = item.llvm_diagnostic or item.diagnostic
                if not diagnostic:
                    raise AssertionError(f"{item.case_id}: reject case lacks diagnostic")
                if rc == 0 or rc < 0 or out:
                    raise AssertionError(
                        f"{item.case_id}: rejection must occur before LLVM IR; rc={rc}")
                if diagnostic.encode() not in err:
                    raise AssertionError(
                        f"{item.case_id}: expected {diagnostic!r}, stderr={err!r}")
                print(f"PASS model reject {item.case_id}: {diagnostic}", flush=True)
                continue

            if rc != 0 or not out.startswith(b"; ModuleID"):
                raise AssertionError(
                    f"{item.case_id}: compiler rc={rc}, stderr={err!r}")

            # INT64_MIN and any future result outside an 8-bit process status
            # are compile-only until the native test ABI gains a lossless i64
            # return transport.
            if item.llvm_mode == "compile_only":
                if item.case_id == "price-adjust-i64-minimum" and                         b"-9223372036854775808" not in out:
                    raise AssertionError(
                        f"{item.case_id}: signed i64 minimum missing from LLVM IR")
                print(f"PASS model compile-only {item.case_id}", flush=True)
                continue

            if item.llvm_mode != "execute":
                raise AssertionError(
                    f"{item.case_id}: unknown LLVM corpus mode {item.llvm_mode!r}")
            if item.semantic_result is None:
                raise AssertionError(
                    f"{item.case_id}: execute case lacks semantic_result_i64")
            if not 0 <= item.semantic_result <= 255:
                raise AssertionError(
                    f"{item.case_id}: process-exit result is not lossless: "
                    f"{item.semantic_result}")

            ll = root / f"{item.case_id}.ll"
            exe = root / item.case_id
            ll.write_bytes(out)
            rc, _, err = run([
                str(clang), "-Wno-override-module", "-x", "ir",
                str(ll), "-o", str(exe)
            ])
            if rc != 0:
                raise AssertionError(
                    f"{item.case_id}: clang rc={rc}, stderr={err!r}")
            rc, _, err = run([str(exe)])
            if rc != item.semantic_result:
                raise AssertionError(
                    f"{item.case_id}: guest exit={rc}, "
                    f"expected={item.semantic_result}, stderr={err!r}")
            print(
                f"PASS model guest {item.case_id}: {item.semantic_result}",
                flush=True
            )


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    ap.add_argument("--corpus", type=Path, required=True)
    ns = ap.parse_args()
    verify(ns.compiler.resolve(), ns.clang.resolve(), ns.corpus.resolve())
