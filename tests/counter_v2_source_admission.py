#!/usr/bin/env python3
"""Fail-closed, source-only admission for the identical Java/LLVM/.NET Counter v2 corpus.

This does NOT compile or execute guest Atto code. The executable parity gate
must additionally compile the materialized sources with the production compiler.
"""
import hashlib
import json
import pathlib
import sys


def git_blob_sha(data: bytes) -> str:
    return hashlib.sha1(b"blob " + str(len(data)).encode("ascii") + b"\0" + data).hexdigest()


def verify(document: dict) -> None:
    required = {
        ".atto.pkg.toml",
        *(f"src/counter/{name}.atto" for name in (
            "actor", "contract", "main", "method", "receive", "state"
        )),
    }
    if set(document) != {
        "schemaVersion", "id", "status", "authority", "origin", "targets",
        "expected", "sourceUnitBlobs", "sourceUnits", "proofRequirements",
        "verifiedBackends",
    }:
        raise ValueError("unknown or missing contract top-level keys")
    if document["schemaVersion"] != 1 or document["id"] != "actor-counter-identical-source-v1":
        raise ValueError("wrong contract schema")
    if document["targets"] != ["java", "llvm", "net"]:
        raise ValueError("three backend identities required")
    if document["status"] != "CANDIDATE_NOT_EXECUTED" or document["verifiedBackends"] != []:
        raise ValueError("source-only admission must not claim backend execution")
    if set(document["sourceUnits"]) != required or set(document["sourceUnitBlobs"]) != required:
        raise ValueError("exactly seven canonical package source units required")
    if document["expected"] != {
        "stdout": "1|3|6|10", "exitCode": 0, "messages": [1, 2, 3, 4],
        "actorStateFinal": 10,
    }:
        raise ValueError("unrecognized Counter v2 oracle")
    for path in sorted(required):
        source = document["sourceUnits"][path]
        if not isinstance(source, str) or not source or "\x00" in source:
            raise ValueError("invalid source unit: " + path)
        raw = source.encode("utf-8", errors="strict")
        if len(raw) > 65_536 or raw.startswith(b"\xef\xbb\xbf"):
            raise ValueError("oversized or BOM source: " + path)
        if git_blob_sha(raw) != document["sourceUnitBlobs"][path]:
            raise ValueError("Git blob SHA mismatch: " + path)
    if "external mtd (Counter c) receive(ActorMail<int> mail)" not in document["sourceUnits"]["src/counter/receive.atto"]:
        raise ValueError("external typed actor ingress absent")
    receiver = document["sourceUnits"]["src/counter/receive.atto"]
    if "val int snapshot = self.handle_state(critical-read |s| -> s.total);" not in receiver:
        raise ValueError("borrow must be detached before mailbox send")
    if "self.send(snapshot);" not in receiver or "self.send(s.total)" in receiver:
        raise ValueError("borrowed state may not cross send boundary")


def main() -> int:
    path = pathlib.Path(__file__).resolve().parents[1] / "contracts" / "actor-counter-identical-source-v1" / "corpus.json"
    data = path.read_bytes()
    if len(data) > 65_536 or data.startswith(b"\xef\xbb\xbf"):
        raise ValueError("bad corpus bytes")
    document = json.loads(data.decode("utf-8"), object_pairs_hook=unique_pairs)
    verify(document)
    for path in sorted(document["sourceUnits"]):
        changed = json.loads(data)
        changed["sourceUnits"][path] += "// mutation\n"
        try:
            verify(changed)
        except ValueError:
            continue
        raise AssertionError("tampered source escaped blob verification: " + path)
    changed = json.loads(data)
    changed["sourceUnitBlobs"].pop("src/counter/main.atto")
    try:
        verify(changed)
    except ValueError:
        pass
    else:
        raise AssertionError("missing source hash accepted")
    print("LLVM_COUNTER_V2_SOURCE_ADMISSION_PASS units=7 negative_mutations=8 guest_executed=false")
    return 0


def unique_pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON key: " + key)
        result[key] = value
    return result


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (ValueError, AssertionError, UnicodeError) as exc:
        print("LLVM_COUNTER_V2_SOURCE_ADMISSION_FAIL:", exc, file=sys.stderr)
        sys.exit(1)
