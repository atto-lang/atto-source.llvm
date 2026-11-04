#!/usr/bin/env python3
"""Emit metadata-only LLVM actor guest evidence after real execution, never pre-certify."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

LEGACY = "AOT_COMPILED_TWO_ACTOR_OWNED_HEAPS_2009_TURNS_PASS"
SEALED = "AOT_COMPILED_SEALED_TWO_ACTOR_OWNED_HEAPS_2009_TURNS_PASS"
SOURCES = (
    "src/main/cpp/oreslang/frontend/Parser.cpp",
    "src/main/cpp/oreslang/frontend/Parser.hpp",
    "src/main/cpp/oreslang/frontend/ModuleLoader.cpp",
    "src/main/cpp/oreslang/backend/LlvmCodegen.cpp",
    "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c",
    "src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.h",
    "tests/native_aot_two_actor_owned_counter_harness.c",
    "tests/fixtures/native-aot-two-actor-owned-counter.atto",
    "tests/fixtures/native-aot-sealed-two-actor-owned-counter.atto",
    ".github/workflows/state-region-actor-lifetime-exact-head.yml",
    "tests/emit_sealed_actor_receipt.py",
)
REQUIRED_CALLS = (
    "ores_actor_i64_heap_scoped_new",
    "ores_actor_i64_heap_scoped_drop",
    "ores_actor_i64_heap_field_read",
    "ores_actor_i64_heap_field_write",
)


def git(*args):
    return subprocess.check_output(("git", *args), text=True).strip()


def sha256(path):
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def check_output(path, expected):
    # One exact line: a marker appended after a program crash, duplicated or
    # accompanied by forged output is not sufficient for an execution receipt.
    if path.read_text(encoding="utf-8").splitlines() != [expected]:
        raise ValueError("native AOT guest receipt missing, duplicated or polluted")


def check_ir(path, handler):
    ir = path.read_text(encoding="utf-8")
    for symbol in ("oreslang_actor_Worker_init", handler):
        if not re.search(r"(?m)^\s*define [^\n]*@" + re.escape(symbol) + r"\(", ir):
            raise ValueError("missing LLVM actor function definition: " + symbol)
    other = ("oreslang_actor_Worker_handle_state" if handler.endswith("on_message")
             else "oreslang_actor_Worker_on_message")
    if re.search(r"(?m)^\s*define [^\n]*@" + re.escape(other) + r"\(", ir):
        raise ValueError("wrong actor handler ABI defined")
    for symbol in REQUIRED_CALLS:
        # A semicolon comment containing 'call @symbol' must never count as
        # evidence. Require a real LLVM call instruction at the line start,
        # permitting only an SSA assignment and optional tail-call prefix.
        instruction = (r"(?m)^[ \t]*(?:%[-A-Za-z0-9_.$]+[ \t]*=[ \t]*)?"
                       r"(?:(?:tail|musttail|notail)[ \t]+)?call[ \t]+[^;\n]*@"
                       + re.escape(symbol) + r"\(")
        if not re.search(instruction, ir):
            raise ValueError("missing LLVM native ownership call: " + symbol)


def build_receipt(root, mode, expected_head, env):
    if mode not in ("optimized", "asan-ubsan"):
        raise ValueError("invalid AOT mode")
    if not re.fullmatch(r"[0-9a-f]{40}", expected_head):
        raise ValueError("invalid head SHA")
    if git("rev-parse", "HEAD") != expected_head:
        raise ValueError("checked-out source head differs from executed head")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("source tree changed during compiled guest validation")
    run_id = int(env["GITHUB_RUN_ID"])
    attempt = int(env["GITHUB_RUN_ATTEMPT"])
    if run_id <= 0 or attempt <= 0 or not env["GITHUB_REPOSITORY"]:
        raise ValueError("missing exact workflow identity")
    outputs = {
        "legacy": ("compiled-two-actor-owned-counter.ll", "aot-two-actor-test",
                   "aot-two-actor-receipt", LEGACY,
                   "oreslang_actor_Worker_on_message"),
        "sealed": ("compiled-sealed-actor-counter.ll", "aot-sealed-two-actor-test",
                   "aot-sealed-two-actor-receipt", SEALED,
                   "oreslang_actor_Worker_handle_state"),
    }
    digests = {}
    for kind, (ir, binary, stdout, marker, handler) in outputs.items():
        check_output(root / stdout, marker)
        check_ir(root / ir, handler)
        digests[kind] = {"ir_sha256": sha256(root / ir),
                         "native_executable_sha256": sha256(root / binary),
                         "native_object_sha256": sha256(root / ("guest.o" if kind == "legacy" else "guest-sealed.o")),
                         "exact_execution_marker": marker}
    return {
        "schema": "attolang.llvm.sealed_scalar_actor_aot_guest_v1",
        "head_sha": expected_head,
        "tree_sha": git("rev-parse", "HEAD^{tree}"),
        "repository": env["GITHUB_REPOSITORY"],
        "run_id": run_id,
        "run_attempt": attempt,
        "job": env.get("GITHUB_JOB", ""),
        "mode": mode,
        "source_blobs": {p: git("rev-parse", "HEAD:" + p) for p in SOURCES},
        "compiled_guest_execution_verified": True,
        "negative_admission_checks_in_same_job": True,
        "experimental_sealed_scalar_state_only": True,
        "asan_ubsan_mode": mode == "asan-ubsan",
        "full_state_intrinsic_parity_verified": False,
        "zero_gc_full_backend_verified": False,
        "executions": digests,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner-temp", type=Path, required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--expected-head", required=True)
    args = parser.parse_args()
    receipt = build_receipt(args.runner_temp, args.mode, args.expected_head, os.environ)
    target = args.runner_temp / ("aot-guest-receipt-" + args.mode + ".json")
    target.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n", encoding="utf-8")
    print("AOT_GUEST_RECEIPT_WRITTEN=" + target.name)


if __name__ == "__main__":
    main()
