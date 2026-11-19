#!/usr/bin/env python3
"""Verify 50 original LLVM source blobs and seven reviewed Atto compiler/runtime overlays.

The overlay contains immutable Git blob IDs from reviewed compiler merges
#10, #13, #14 and #15, plus the caret-return semantic merge of #19 and the #21 tenant heap hardening.
The original 57-blob provenance manifest remains unchanged.
This does not certify full source migration or complete Atto parity.
"""
import json
import os
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
OVERLAY = json.loads((ROOT / "ci/accepted-llvm-source-overlays.json").read_text())["files"]
EXPECTED_OVERLAY_PATHS = set(["src/main/cpp/oreslang/backend/LlvmCodegen.cpp","src/main/cpp/oreslang/frontend/Parser.cpp","src/main/cpp/oreslang/frontend/Parser.hpp","src/main/cpp/oreslang/main.cpp","src/main/cpp/oreslang/runtime/TenantVm.cpp","src/main/cpp/oreslang/runtime/TenantVmGuestAbi.h","src/main/cpp/oreslang/types/OwnershipChecker.cpp"])
ORIGIN = "b16089183aa1864adaef5bb0a13051d19430db09"


def git(*args):
    return subprocess.check_output(["git", *args], cwd=ROOT, text=True).strip()


def verify():
    if set(OVERLAY) != EXPECTED_OVERLAY_PATHS or any(
            not re.fullmatch(r"[0-9a-f]{40}", sha) for sha in OVERLAY.values()):
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: invalid reviewed overlay manifest")
    expected = os.environ.get("ATTO_GUARD_HEAD_SHA", "")
    if not re.fullmatch(r"[0-9a-f]{40}", expected) or git("rev-parse", "HEAD") != expected:
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: exact Git head mismatch")
    if git("status", "--porcelain", "--untracked-files=all"):
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: worktree is not clean")
    origin = git("rev-parse", f"{ORIGIN}^{{commit}}")
    if origin != ORIGIN:
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: original parent commit missing")
    ancestor = subprocess.run(["git", "merge-base", "--is-ancestor", ORIGIN, expected],
                              cwd=ROOT, check=False)
    if ancestor.returncode != 0:
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: source commit not descendant of parent")
    manifest = json.loads((ROOT / "ci/source-subset-manifest.json").read_text())
    if manifest.get("file_count") != 57 or len(manifest.get("files", {})) != 57:
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: expected original 57-file manifest")
    if not set(OVERLAY).issubset(manifest["files"]):
        raise ValueError("ATTO_GUARD_NOT_EXECUTED: changed path absent in provenance")
    for path, original_blob in sorted(manifest["files"].items()):
        file = ROOT / path
        if not file.is_file() or file.is_symlink():
            raise ValueError("ATTO_GUARD_NOT_EXECUTED: source file missing or symlink")
        if not re.fullmatch(r"[0-9a-f]{40}", original_blob):
            raise ValueError("ATTO_GUARD_NOT_EXECUTED: malformed original blob")
        blob = git("hash-object", "--", str(file))
        if blob != OVERLAY.get(path, original_blob):
            raise ValueError("ATTO_GUARD_NOT_EXECUTED: unexpected source blob " + path)
        if path in OVERLAY and original_blob == blob:
            raise ValueError("ATTO_GUARD_NOT_EXECUTED: compiler patch accidentally reverted")
    for p in OVERLAY:
        old_blob = git("rev-parse", f"{ORIGIN}:{p}")
        if manifest["files"][p] != old_blob:
            raise ValueError("ATTO_GUARD_NOT_EXECUTED: inaccurate immutable original")
    print("ATTO_GUARD_COMPILER_HEAD=" + expected)
    print("ATTO_GUARD_COMPILER_ROOT_TREE=" + git("rev-parse", "HEAD^{tree}"))
    print("ORIGINAL_UNCHANGED_GIT_BLOBS=50")
    print("REVIEWED_ATTO_SOURCE_BLOBS=7")
    print("SOURCE_COMPLETE_MIGRATION=false")


if __name__ == "__main__":
    try:
        verify()
    except (ValueError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as e:
        print(str(e), file=sys.stderr)
        sys.exit(1)
