#!/usr/bin/env python3
"""Fail-closed exact-tree LLVM scalar guest AOT certification runner.

Designed for an approved, isolated runner with a *complete* source checkout.
This script never authenticates to a source organization, mirrors code, or
changes source tree contents. An omitted or skipped test is NEVER certified.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

TESTS = (
    "native_metered_cfg_cycles_fail_closed",
    "native_scalar_guest_calls_fail_closed",
    "native_scalar_ir_surface_scan",
    "native_scalar_ir_surface_scan_python_optimized",
    "native_reduction_quantum_watchdog_host_only",
    "native_rt_scalar_frame_aot_two_tasks",
)
TARGETS = (
    "oreslang-llvmc",
    "oreslang-native-metered-cfg-verifier-test",
    "oreslang-native-scalar-call-verifier-test",
    "oreslang-native-reduction-quantum-test",
)
SHA = re.compile(r"[0-9a-f]{40}\Z")

class GateFailed(RuntimeError):
    pass


def command(args, *, cwd=None, timeout=600, log=None):
    try:
        # CTest Python imports must not create __pycache__ inside the audited
        # source checkout: that would invalidate the post-execution proof.
        child_env = os.environ.copy()
        child_env["PYTHONDONTWRITEBYTECODE"] = "1"
        p = subprocess.run(args, cwd=cwd, timeout=timeout, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           check=False, env=child_env)
    except (OSError, subprocess.TimeoutExpired) as e:
        raise GateFailed(f"command unavailable or timed out: {args[0]}: {e}") from e
    if log is not None:
        log.write_text("COMMAND: " + " ".join(map(str, args)) + "\n" + p.stdout,
                       encoding="utf-8")
    if p.returncode != 0:
        raise GateFailed(f"command exited {p.returncode}: {args[0]} (see receipt/log)")
    return p.stdout.strip()


def git(repo, *args):
    return command(["git", "-C", str(repo), *args], timeout=120)


def blob_hash(path: Path, mode: str) -> str:
    # Hash the *actual worktree bytes*, not Git's index/stat cache. Reject
    # assume-unchanged tricks, submodules, symlink traversal and mode changes.
    if mode == "120000":
        if not path.is_symlink():
            raise GateFailed(f"expected symlink is not symlink: {path}")
        value = os.fsencode(os.readlink(path))
        return hashlib.sha1(b"blob " + str(len(value)).encode() + b"\0" + value).hexdigest()
    if mode not in ("100644", "100755") or not path.is_file() or path.is_symlink():
        raise GateFailed(f"invalid regular Git mode or worktree path: {path}")
    actual_executable = bool(path.stat().st_mode & (stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH))
    if actual_executable != (mode == "100755"):
        raise GateFailed(f"tracked executable mode changed: {path}")
    size = path.stat().st_size
    h = hashlib.sha1()
    h.update(b"blob " + str(size).encode() + b"\0")
    with path.open("rb") as file:
        for chunk in iter(lambda: file.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def verify_checkout(repo: Path, expected_head: str) -> dict:
    if not SHA.fullmatch(expected_head):
        raise GateFailed("expected HEAD must be a full 40-character SHA-1 commit")
    repo = repo.resolve(strict=True)
    root = Path(git(repo, "rev-parse", "--show-toplevel")).resolve()
    if root != repo:
        raise GateFailed("--repo must point to the Git checkout root")
    if git(repo, "rev-parse", "--show-object-format") != "sha1":
        raise GateFailed("only the GitHub SHA-1 repository format is supported")
    actual_head = git(repo, "rev-parse", "HEAD")
    if actual_head != expected_head:
        raise GateFailed(f"HEAD mismatch: {actual_head}; expected {expected_head}")
    if git(repo, "status", "--porcelain=v1", "--untracked-files=all"):
        raise GateFailed("checkout is dirty or includes untracked files")
    tree = git(repo, "rev-parse", "HEAD^{tree}")
    raw = subprocess.run(["git", "-C", str(repo), "ls-tree", "--full-tree", "-rz", "HEAD"],
                         stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120, check=False)
    if raw.returncode:
        raise GateFailed("cannot enumerate the exact Git commit tree")
    blobs = 0
    for entry in raw.stdout.split(b"\0"):
        if not entry:
            continue
        try:
            metadata, name = entry.split(b"\t", 1)
            mode, kind, expected = metadata.decode("ascii").split(" ")
        except (ValueError, UnicodeDecodeError) as e:
            raise GateFailed("malformed Git ls-tree record") from e
        if kind != "blob":
            raise GateFailed("non-blob Git entries (including submodules) are not admitted")
        path = repo / os.fsdecode(name)
        if blob_hash(path, mode) != expected:
            raise GateFailed(f"worktree blob mismatch: {os.fsdecode(name)}")
        blobs += 1
    if blobs == 0:
        raise GateFailed("source tree is empty")
    return {"head": actual_head, "tree": tree, "verified_blobs": blobs,
            "clean_worktree": True, "non_blob_entries": 0}


def major(text: str) -> int:
    match = re.fullmatch(r"\s*([0-9]+)(?:\.[0-9]+)*\s*", text)
    if not match:
        raise GateFailed("LLVM/Clang version could not be parsed exactly")
    return int(match.group(1))


def verify_toolchain(llvm_config: str, clang: str) -> dict:
    llvm = command([llvm_config, "--version"], timeout=30)
    clang_version = command([clang, "-dumpversion"], timeout=30)
    if major(llvm) != major(clang_version):
        raise GateFailed(f"LLVM SDK {llvm} != Clang {clang_version}: do not certify")
    cmake_dir = command([llvm_config, "--cmakedir"], timeout=30)
    if not Path(cmake_dir).is_dir():
        raise GateFailed("llvm-config --cmakedir does not identify an LLVM CMake SDK")
    for exe, args in (("cmake", ["--version"]), ("ninja", ["--version"])):
        command([exe, *args], timeout=30)
    return {"llvm": llvm, "clang": clang_version, "llvm_cmake_dir": cmake_dir,
            "clang_binary": clang, "llvm_config_binary": llvm_config}


def verify_junit(path: Path) -> dict:
    if not path.is_file():
        raise GateFailed("CTest JUnit report missing: test execution is NOT_EXECUTED")
    try:
        doc = ET.parse(path)
    except ET.ParseError as e:
        raise GateFailed("CTest JUnit report malformed") from e
    cases = doc.findall(".//testcase")
    observed = {}
    for case in cases:
        name = case.get("name")
        if name in observed:
            raise GateFailed(f"duplicate CTest result for {name}")
        observed[name] = not any(case.find(k) is not None for k in ("failure", "error", "skipped"))
    if set(observed) != set(TESTS) or not all(observed.values()):
        raise GateFailed("missing, extra, skipped or failed native CTest case")
    return {"executed_cases": len(cases), "names": sorted(observed), "status": "PASS"}


def certify(repo: Path, expected_head: str, llvm_config: str, clang: str, out: Path) -> dict:
    # Validate the full source tree BEFORE any output is created. This also
    # protects source worktrees from side effects of a failed provenance gate.
    source = verify_checkout(repo, expected_head)
    toolchain = verify_toolchain(llvm_config, clang)
    out = out.resolve()
    if repo.resolve() in out.parents or out == repo.resolve():
        raise GateFailed("artifacts must be stored outside the source checkout")
    if out.exists() and (not out.is_dir() or any(out.iterdir())):
        raise GateFailed("evidence output directory must be new/empty")
    out.mkdir(parents=True, exist_ok=True)
    receipt = {"status": "NOT_CERTIFIED", "source": source, "toolchain": toolchain,
               "steps": [], "tests": "NOT_EXECUTED", "generated_at": time.time()}
    def save():
        (out / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    save()
    build = out / "build"
    steps = [
        ("configure", ["cmake", "-S", str(repo), "-B", str(build), "-G", "Ninja",
                       "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
                       f"-DLLVM_DIR={toolchain['llvm_cmake_dir']}"]),
        ("build", ["cmake", "--build", str(build), "--target", *TARGETS]),
    ]
    try:
        for name, cmd in steps:
            command(cmd, timeout=1200, log=out / (name + ".log"))
            receipt["steps"].append({"step": name, "executed": True, "status": "PASS"})
            save()
        found = json.loads(command(["ctest", "--test-dir", str(build), "--show-only=json-v1"],
                                   log=out / "discovery.log"))
        present = {test.get("name") for test in found.get("tests", [])}
        if not set(TESTS).issubset(present):
            raise GateFailed("required security CTest targets absent from configured tree")
        junit = out / "ctest.xml"
        expression = "^(?:" + "|".join(re.escape(name) for name in TESTS) + ")$"
        command(["ctest", "--test-dir", str(build), "--output-on-failure", "--output-junit",
                 str(junit), "-R", expression], timeout=1800, log=out / "ctest.log")
        receipt["tests"] = verify_junit(junit)
        # A source checkout can change while a long native build is running.
        # Require the exact original head/tree/blobs again AFTER all tests;
        # an exclusive isolated runner remains mandatory against TOCTOU.
        post_source = verify_checkout(repo, expected_head)
        if post_source != source:
            raise GateFailed("source checkout changed during native certification")
        receipt["post_build_source"] = post_source
        for binary in (build / "oreslang-llvmc", build / "oreslang-native-scalar-call-verifier-test"):
            if not binary.is_file():
                raise GateFailed(f"required native test binary missing: {binary.name}")
        receipt["binaries"] = {
            b.name: hashlib.sha256(b.read_bytes()).hexdigest()
            for b in (build / "oreslang-llvmc", build / "oreslang-native-scalar-call-verifier-test")}
        # The proof receipt names the exact command/CTest evidence. Hash the
        # underlying logs and test report so later tampering is detectable.
        evidence_files = ("configure.log", "build.log", "discovery.log", "ctest.log", "ctest.xml")
        for name in evidence_files:
            if not (out / name).is_file():
                raise GateFailed(f"required executed evidence file missing: {name}")
        receipt["evidence_sha256"] = {
            name: hashlib.sha256((out / name).read_bytes()).hexdigest()
            for name in evidence_files}
        receipt["status"] = "EXACT_HEAD_EXECUTED_PASS"
        save()
        return receipt
    except (GateFailed, ValueError, json.JSONDecodeError) as e:
        receipt["status"] = "FAILED_OR_NOT_EXECUTED"
        receipt["first_actionable_failure"] = str(e)
        save()
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--llvm-config", default="llvm-config")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = certify(args.repo, args.expected_head, args.llvm_config, args.clang, args.out)
    except GateFailed as e:
        print(f"NOT_CERTIFIED: {e}", file=sys.stderr)
        return 2
    print(json.dumps({"status": result["status"], "tests": result["tests"]}, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
