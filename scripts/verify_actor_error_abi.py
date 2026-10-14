#!/usr/bin/env python3
"""Validate the frozen v1 actor error vocabulary against guest and host declarations.

This is a static ABI gate, not proof of guest typechecking, effect lowering,
actor-supervisor routing or runtime serializer safety.
"""
import argparse
import csv
import pathlib
import re
import sys

EXPECTED = [
    ("ActorStoppedError", "stopped", True),
    ("ActorMailboxFullError", "mailbox_full", True),
    ("ActorMailboxClosedError", "mailbox_closed", True),
    ("ActorSendRejectedError", "send_rejected", True),
    ("ActorTransportError", "transport", True),
    ("ActorIsolationError", "isolation", False),
    ("ActorCapabilityRevokedError", "capability_revoked", False),
    ("ActorSpawnError", "spawn", True),
    ("ActorSupervisionError", "supervision", False),
    ("ActorRestartLimitError", "restart_limit", False),
    ("ActorDeadlineExceededError", "deadline_exceeded", False),
    ("ActorCancelledError", "cancelled", False),
    ("ActorLeaseExpiredError", "lease_expired", False),
    ("ActorFencingError", "fencing", False),
    ("ActorGroupClosedError", "group_closed", True),
    ("ActorGroupPermitError", "group_permit", False),
    ("ActorRemoteUnavailableError", "remote_unavailable", True),
    ("ActorSerializationError", "serialization", True),
    ("ActorHeapExhaustedError", "heap_exhausted", False),
    ("ActorRuntimeInvariantError", "runtime_invariant", False),
]
ERROR_CLASSES = [
    "ArgumentError", "TypeMismatchError", "IndexOutOfBoundsError",
    "PermissionDeniedError", "TimeoutError", "CancellationError",
    "NetworkError", "FileSystemError", "EncodingError", "SerializationError",
    "ParseError", "ResourceLimitError", "InvariantError",
]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--root", type=pathlib.Path, required=True)
    p.add_argument("--backend", choices=["java", "llvm"], required=True)
    args = p.parse_args()
    root = args.root.resolve()
    guest = root / ("stdlib" if args.backend == "java" else "src/core")
    actor_text = (guest / "actor_errors.ores").read_text(encoding="utf-8")
    error_text = (guest / "errors.ores").read_text(encoding="utf-8")
    with (root / "contracts/v1/actor-errors.csv").open(newline="", encoding="utf-8") as f:
        rows = list(csv.DictReader(f))
    require(len(rows) == len(EXPECTED), "actor error catalog size changed")
    expected_fields = {"symbol", "code", "key", "description", "guest_boundary"}
    require(set(rows[0]) == expected_fields, "unexpected catalog columns")
    require(bool(re.search(r"\bclass\s+ActorError\s+extends\s+Error\s+as\b", actor_text)),
            "missing nominal ActorError inheritance declaration")
    require(bool(re.search(r"\bclass\s+RuntimeError\s+extends\s+Error\s+as\b", error_text)),
            "missing RuntimeError inheritance declaration")
    for name in ERROR_CLASSES:
        require(bool(re.search(r"\bclass\s+" + name +
                        r"\s+extends\s+RuntimeError\s+as\b", error_text)),
                "missing generic Error subclass: " + name)

    host_path = root / ("src/main/java/dev/oreslang/runtime/ActorErrorCatalog.java"
                        if args.backend == "java" else
                        "src/main/cpp/oreslang/runtime/ActorErrorCatalog.hpp")
    host = host_path.read_text(encoding="utf-8")
    for i, ((name, wire, recoverable), row) in enumerate(zip(EXPECTED, rows)):
        code = 1001 + i
        require(row["symbol"] == name and row["code"] == str(code) and row["key"] == wire,
                f"v1 ABI code or name changed at {code}")
        require(row["guest_boundary"] == ("recoverable" if recoverable else "terminal"),
                f"v1 policy changed at {code}")
        require(row["description"].strip() and "," not in row["description"],
                f"invalid code {code} description")
        require(bool(re.search(r"\bclass\s+" + name +
                    r"\s+extends\s+ActorError\s+as\b", actor_text)),
                f"guest class missing: {name}")
        require(f"// {code} {wire}:" in actor_text, f"missing guest code metadata {code}")
        if args.backend == "java":
            enum_name = re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", name.removesuffix("Error")).upper()
            pattern = (r"\b" + enum_name + r"\s*\(\s*" + str(code) +
                       r'\s*,\s*"' + wire + r'"\s*,\s*' +
                       str(recoverable).lower() + r"\s*\)")
        else:
            enum_name = name.removesuffix("Error")
            pattern = r"\b" + enum_name + r"\s*=\s*" + str(code) + r"\s*,"
            policy_pattern = (r"case\s+ActorErrorCode::" + enum_name +
                              r'\s*:\s*return\s*\{\s*code\s*,\s*"' + wire +
                              r'"\s*,\s*' + str(recoverable).lower() + r"\s*\}")
            require(re.search(policy_pattern, host), f"native policy mismatch: {name}")
        require(re.search(pattern, host), f"host catalog mismatch: {name}")
    print(f"{args.backend}: 20 frozen actor errors and 13 core errors match source+host catalog")
    print("NOTE: guest Error inheritance, effect lowering and supervisor serialization are not proven.")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, OSError, ValueError) as exc:
        print(f"actor error ABI verification failed: {exc}", file=sys.stderr)
        sys.exit(1)
