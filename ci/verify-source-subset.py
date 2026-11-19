#!/usr/bin/env python3
"""Verify exactly mirrored source Git blobs before compiling any guest.

A pass proves per-file content identity to the specified original Git commit,
NOT completeness of a source tree nor security/correctness of generated code.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "ci/source-subset-manifest.json"


def check():
    m = json.loads(MANIFEST.read_text())
    assert m["schema"] == "atto.exact-sourced-llvm-tenant-guest-subtree.v1"
    assert m["original"]["repository"] == "ores-truffle-oreslang/oreslang-source.llvm"
    assert re.fullmatch("[0-9a-f]{40}", m["original"]["commit"])
    assert m["file_count"] == len(m["files"]) == 57
    overlay = json.loads((ROOT / "ci/accepted-llvm-source-overlays.json").read_text())
    assert overlay.get("schema") == "atto.llvm.reviewed-compiler-overlays.v1"
    overrides = overlay.get("files")
    assert isinstance(overrides, dict) and set(overrides) == set(["src/main/cpp/oreslang/backend/LlvmCodegen.cpp","src/main/cpp/oreslang/frontend/Parser.cpp","src/main/cpp/oreslang/frontend/Parser.hpp","src/main/cpp/oreslang/main.cpp","src/main/cpp/oreslang/types/OwnershipChecker.cpp"])
    expected = os.environ.get("EXPECTED_CARRIER_HEAD_SHA", "")
    assert re.fullmatch("[0-9a-f]{40}", expected), "carrier checkout SHA must be pinned"
    actual_head = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    assert actual_head == expected, "checked-out carrier commit mismatch"
    subprocess.run(["git", "diff", "--quiet", "HEAD", "--"], cwd=ROOT, check=True)
    untracked = subprocess.check_output(
        ["git", "ls-files", "--others", "--exclude-standard"], cwd=ROOT, text=True
    ).strip()
    assert not untracked, "unexpected untracked files before build: " + untracked[:180]
    for rel, expected_blob in sorted(m["files"].items()):
        path = ROOT / rel
        assert re.fullmatch("[0-9a-f]{40}", expected_blob), rel + ": bad SHA"
        assert path.is_file() and not path.is_symlink(), rel + ": missing/symlinked"
        blob = subprocess.check_output(["git", "hash-object", str(path)], text=True).strip()
        assert blob == overrides.get(rel, expected_blob), rel + ": wrong Git blob or unreviewed overlay"
    print("ORIGINAL_SOURCE_COMMIT="+m["original"]["commit"])
    print("ORIGINAL_SOURCE_GIT_BLOBS_VERIFIED="+str(len(m["files"])-len(overrides)))
    print("REVIEWED_ATTO_COMPILER_BLOBS_VERIFIED="+str(len(overrides)))
    print("SOURCE_TREE_SCOPE=SELECTED_EXACT_SOURCE_BLOBS_NOT_ORIGINAL_COMPLETE_TREE")


if __name__ == "__main__":
    try:
        check()
    except (AssertionError, OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print("ORIGIN_SNAPSHOT_INVALID: "+str(error), file=sys.stderr)
        sys.exit(1)
