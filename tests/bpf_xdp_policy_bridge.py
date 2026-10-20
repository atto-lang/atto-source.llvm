#!/usr/bin/env python3
"""Fail-closed LOCAL XDP build/policy test bridge; NOT export authorization or attestation.

Runs only against a trusted local LLVM compiler and independently installed
pinned target-profile package. Does not copy source between organizations.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile

PACKET = "bpf.packet.read"
MAX_SOURCE = 4096
HEX256 = re.compile(r"[0-9a-f]{64}\Z")

class Denied(Exception):
    pass


def require(ok, why):
    if not ok:
        raise Denied(why)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def hash_file(path):
    hasher = hashlib.sha256()
    with open(path, "rb") as handle:
        for part in iter(lambda: handle.read(128 * 1024), b""):
            hasher.update(part)
    return hasher.hexdigest()


def pinned_bytes(path, expected):
    require(bool(HEX256.fullmatch(expected)), "invalid expected source SHA256")
    if not hasattr(os, "O_NOFOLLOW") or not hasattr(os, "O_NONBLOCK"):
        raise Denied("source file admission requires POSIX nofollow/nonblock")
    fd = os.open(str(path), os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        require(stat.S_ISREG(info.st_mode), "source is not a regular file")
        require(info.st_size <= MAX_SOURCE, "XDP source exceeds 4096 bytes")
        parts = []
        total = 0
        while True:
            part = os.read(fd, min(4096, MAX_SOURCE + 1 - total))
            if not part:
                break
            total += len(part)
            require(total <= MAX_SOURCE, "XDP source exceeds 4096 bytes")
            parts.append(part)
        data = b"".join(parts)
        require(digest(data) == expected, "source SHA256 mismatch")
        return data
    finally:
        os.close(fd)


def strict_pairs(items):
    result = {}
    for key, value in items:
        require(key not in result, "duplicate key in compiler effect closure: " + key)
        result[key] = value
    return result


def strict_json(raw):
    has_bom = raw.startswith(b"\xef\xbb\xbf") if isinstance(raw, bytes) else raw.startswith("\ufeff")
    require(not has_bom, "compiler closure BOM is prohibited")
    def reject_nonfinite(word):
        raise Denied("nonfinite compiler closure value: " + word)
    try:
        return json.loads(raw, object_pairs_hook=strict_pairs,
                          parse_constant=reject_nonfinite)
    except (ValueError, UnicodeDecodeError) as exc:
        raise Denied("malformed compiler closure JSON") from exc


def validate_closure(value):
    require(type(value) is dict and set(value) == {
        "schemaVersion", "entry", "target", "backend", "grants", "nodes"},
        "noncanonical compiler closure root")
    require(value["schemaVersion"] == 1 and type(value["schemaVersion"]) is int,
            "unsupported closure version")
    require((value["entry"], value["target"], value["backend"]) ==
            ("ores_xdp", "ebpf", "llvm"), "wrong XDP target identity")
    require(value["grants"] == [] and type(value["grants"]) is list,
            "compiler may not self-authorize grants")
    nodes = value["nodes"]
    require(type(nodes) is list and len(nodes) == 1 and type(nodes[0]) is dict,
            "XDP closure must contain exactly one node")
    node = nodes[0]
    require(set(node) == {"id", "targets", "requires", "imports"},
            "noncanonical compiler closure node")
    require(node["id"] == "ores_xdp" and type(node["id"]) is str and
            node["targets"] == ["ebpf"] and type(node["targets"]) is list and
            node["imports"] == [] and type(node["imports"]) is list,
            "unexpected XDP compiler imports or targets")
    requirements = node["requires"]
    require(type(requirements) is list and
            requirements in ([], [PACKET]), "unexpected XDP compiler effects")
    return bool(requirements)


def check_bpf_elf(blob):
    require(len(blob) >= 64 and blob[:6] == b"\x7fELF\x02\x01", "not little-endian ELF64")
    require(struct.unpack_from("<HHI", blob, 16) == (1, 247, 1),
            "ELF must be ET_REL/EM_BPF/version 1")


def run_command(args, expected=0, timeout=90):
    try:
        result = subprocess.run(args, capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise Denied("tool unavailable or timeout: " + str(args[0])) from exc
    require(result.returncode == expected,
            "tool failed policy/integrity gate: " + str(args[0]) +
            " status=" + str(result.returncode) +
            " diagnostic=" + result.stderr[:250].decode("utf-8", "replace"))
    return result.stdout


def verify_object_metadata(path, readelf):
    sections = run_command([readelf, "-W", "-S", str(path)]).decode("utf-8", "replace")
    for name in ("xdp", "license"):
        require(len(re.findall(r"\]\s+" + name + r"\s+PROGBITS\b", sections)) == 1,
                "BPF object must contain exactly one " + name + " section")
    require(not any(re.search(r"\s" + re.escape(x) + r"\s", sections)
                    for x in (".maps", ".BTF", ".BTF.ext", ".eh_frame")),
            "unsupported eBPF sections present")
    symbols = run_command([readelf, "-W", "-s", str(path)]).decode("utf-8", "replace")
    globals_seen = set()
    for line in symbols.splitlines():
        cols = line.split()
        if len(cols) >= 8:
            require(cols[6] != "UND" or not cols[7], "unresolved BPF symbol")
            if cols[4] == "GLOBAL" and cols[7]:
                require(cols[7] in ("ores_xdp", "_license"), "unapproved BPF global")
                globals_seen.add(cols[7])
    require(globals_seen == {"ores_xdp", "_license"}, "missing XDP/license symbols")
    reloc = run_command([readelf, "-W", "-r", str(path)]).decode("utf-8", "replace")
    require("There are no relocations in this file." in reloc,
            "BPF object contains unreviewed relocations")


def build(args):
    source_bytes = pinned_bytes(args.source, args.expected_source_sha256)
    require(Path(args.compiler).is_file() and
            hash_file(args.compiler) == args.expected_compiler_sha256,
            "compiler executable SHA256 does not match pinned input")
    require(Path(args.policy_cli).is_file() and
            hash_file(args.policy_cli) == args.expected_policy_cli_sha256,
            "target-policy CLI SHA256 does not match pinned input")
    require(args.grants in ("", PACKET), "unsupported external XDP grants")
    for sha in (args.expected_compiler_sha256, args.expected_policy_cli_sha256):
        require(bool(HEX256.fullmatch(sha)), "invalid executable SHA256 pin")
    with tempfile.TemporaryDirectory(prefix="ores-bpf-policy-local-") as temp:
        base = Path(temp)
        source = base / "source.ores"
        source.write_bytes(source_bytes)
        source.chmod(0o600)
        base_flags = [str(args.compiler), "--target=bpfel", "--bpf-program=xdp"]
        raw = run_command(base_flags + ["--emit-target-closure", str(source)])
        require(len(raw) <= 4096, "compiler effect closure too large")
        packet = validate_closure(strict_json(raw))
        require((PACKET if packet else "") == args.grants,
                "external grants do not exactly equal compiler inferred effects")
        closure = base / "closure.json"
        closure.write_bytes(raw)
        closure.chmod(0o600)
        # The target-profile CLI recomputes effective authority externally;
        # success here is a necessary check, never a self-authorizing grant.
        command = [args.node, str(args.policy_cli), "check-closure", str(closure),
                   "--target=ebpf", "--backend=llvm", "--grants=" + args.grants]
        run_command(command)
        emit_flags = ["--bpf-grants=" + PACKET] if packet else []
        ir = run_command(base_flags + emit_flags + [str(source)])
        require(len(ir) <= 128 * 1024 and b'target triple = "bpfel"' in ir and
                b'section "xdp"' in ir and b'section "license"' in ir,
                "compiler did not emit bounded XDP LLVM IR")
        ll = base / "source.ll"
        ll.write_bytes(ir)
        obj = base / "source.o"
        run_command([args.clang, "--target=bpfel", "-O2", "-x", "ir", "-c",
                     str(ll), "-o", str(obj)])
        blob = obj.read_bytes()
        require(len(blob) <= 2 * 1024 * 1024, "BPF object exceeds build test limit")
        check_bpf_elf(blob)
        verify_object_metadata(obj, args.readelf)
        return {
            "schemaVersion": 1,
            "status": "UNSIGNED_TEST_EVIDENCE_NOT_DEPLOYMENT_AUTHORIZATION",
            "sourceSha256": digest(source_bytes),
            "compilerSha256": args.expected_compiler_sha256,
            "policyCliSha256": args.expected_policy_cli_sha256,
            "closureSha256": digest(raw), "llvmIrSha256": digest(ir),
            "bpfElfSha256": digest(blob),
            "inferredEffects": [PACKET] if packet else [],
            "externalGrants": [PACKET] if packet else [],
            "verifiedByKernel": False,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "compiler", "policy-cli", "expected-source-sha256",
                 "expected-compiler-sha256", "expected-policy-cli-sha256"):
        parser.add_argument("--" + name, required=True)
    parser.add_argument("--grants", default="")
    parser.add_argument("--clang", default="clang")
    parser.add_argument("--readelf", default="readelf")
    parser.add_argument("--node", default="node")
    args = parser.parse_args()
    try:
        print(json.dumps(build(args), sort_keys=True, separators=(",", ":")))
    except (Denied, OSError) as exc:
        print("BPF_POLICY_BRIDGE_DENIED: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
