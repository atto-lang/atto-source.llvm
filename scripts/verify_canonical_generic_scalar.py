#!/usr/bin/env python3
"""Run the merged canonical scalar-generic contract against the LLVM compiler.

The corpus and executable runner live ONLY in oreslang-common-source. Require
an exact Git tree and suite digest before trusting or executing those files.
Use --common-source-root on a separate checkout or in an exact-tree carrier.
"""
import argparse
from pathlib import Path
import subprocess
import sys
from pinned_generic_worktree import attest_pinned_worktree

COMMON_ROOT_TREE = "858c37d48a4c3a886260b26320bb0dd931b228ab"
COMMON_CONTRACT_TREE = "fd5301ebd354049917e2ecaf1da920b661d6d829"
COMMON_SUITE_DIGEST = "80a5053f8ba147346793a209e4f8ec04b8daf4cedb6a149fb1d127ea4af4ffb5"


def call(command, timeout=30):
    return subprocess.run(command, capture_output=True, text=True,
                          timeout=timeout, check=False)


def prove_canonical_source(common_root):
    contract = common_root / "contracts/v1/generic-scalar"
    runner = contract / "verify.py"
    if not runner.is_file() or runner.is_symlink():
        raise ValueError("missing or symlinked authoritative generic verifier")
    try:
        git_root = Path(call(["git", "-C", str(common_root), "rev-parse",
                              "--show-toplevel"]).stdout.strip()).resolve(strict=True)
        rel = contract.relative_to(git_root)
    except (ValueError, OSError) as exc:
        raise ValueError("canonical source must be inside a real Git checkout") from exc
    full_source_rel = common_root.relative_to(git_root)
    source_rev = "HEAD^{tree}" if not full_source_rel.parts else "HEAD:" + full_source_rel.as_posix()
    root_proof = call(["git", "-C", str(git_root), "rev-parse", source_rev])
    if root_proof.returncode or root_proof.stdout.strip() != COMMON_ROOT_TREE:
        raise ValueError("canonical common-source full-tree SHA mismatch")
    rev = call(["git", "-C", str(git_root), "rev-parse", "HEAD:" + rel.as_posix()])
    if rev.returncode or rev.stdout.strip() != COMMON_CONTRACT_TREE:
        raise ValueError("canonical common-source contract tree SHA mismatch")
    attest_pinned_worktree(git_root, common_root, COMMON_ROOT_TREE)
    dirty = call(["git", "-C", str(git_root), "status", "--porcelain=v1",
                  "--untracked-files=all", "--", full_source_rel.as_posix() if full_source_rel.parts else "."])
    if dirty.returncode or dirty.stdout.strip():
        raise ValueError("canonical generic fixtures are modified or untracked")
    checked = call([sys.executable, "-B", str(runner), "--validate-only"], timeout=20)
    proof = ("GENERIC_SCALAR_SUITE_SHA256=" + COMMON_SUITE_DIGEST + "; cases=20")
    if checked.returncode or proof not in checked.stdout:
        raise ValueError("canonical generic suite digest/count verification failed")
    return runner


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--common-source-root", type=Path, required=True)
    ap.add_argument("--compiler", type=Path, required=True)
    ap.add_argument("--clang", type=Path, required=True)
    args = ap.parse_args()
    try:
        runner = prove_canonical_source(args.common_source_root.resolve(strict=True))
        binary = args.compiler.resolve(strict=True)
        clang = args.clang.resolve(strict=True)
        result = subprocess.run([sys.executable, "-B", str(runner),
                                 "--backend", "llvm", "--compiler", str(binary),
                                 "--clang", str(clang)], check=False,
                                timeout=400)
        return result.returncode
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"CANONICAL_GENERIC_GATE_FAILED: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
