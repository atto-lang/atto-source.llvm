#!/usr/bin/env python3
"""Source-only, fail-closed GitHub required-check identity verifier.

The expected contract must originate from immutable, reviewed source main.
Never learn expected check identities from the observed protection response.
"""
import json
import re
import sys
from pathlib import Path

REPOSITORY = "ores-truffle-oreslang/oreslang-source.llvm"
CONTEXT = re.compile(r"[A-Za-z0-9][A-Za-z0-9_./ :+-]{0,127}\Z")


class Denied(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise Denied(reason)


def pairs(value, label):
    require(type(value) is list and value, label + "-missing")
    entries = []
    for item in value:
        require(type(item) is dict and set(item) == {"context", "app_id"},
                label + "-invalid-entry")
        name, app = item["context"], item["app_id"]
        require(type(name) is str and CONTEXT.fullmatch(name) is not None,
                label + "-invalid-context")
        require(type(app) is int and app > 0, label + "-invalid-app-id")
        entries.append((name, app))
    require(len(entries) == len(set(entries)), label + "-duplicate-pair")
    require(len(entries) == len({name for name, _ in entries}),
            label + "-duplicate-context")
    return set(entries)


def verify(pins, protection):
    require(type(pins) is dict and set(pins) ==
            {"version", "repository", "branch", "state", "checks"},
            "invalid-contract-shape")
    require(type(pins["version"]) is int and pins["version"] == 1,
            "unsupported-contract-version")
    require(pins["repository"] == REPOSITORY and pins["branch"] == "main",
            "wrong-contract-source")
    require(pins["state"] == "APPROVED", "independent-pins-not-approved")
    expected = pairs(pins["checks"], "approved-pins")
    require(type(protection) is dict, "invalid-protection-response")
    required = protection.get("required_status_checks")
    require(type(required) is dict and required.get("strict") is True,
            "strict-required-checks-missing")
    actual = pairs(required.get("checks"), "observed-required-checks")
    require(actual == expected, "unapproved-check-context-or-app-id")
    contexts = required.get("contexts")
    require(type(contexts) is list and all(type(c) is str for c in contexts),
            "legacy-contexts-invalid")
    require(len(contexts) == len(set(contexts)), "duplicate-legacy-context")
    # GitHub REST can include the mirrored legacy contexts alongside checks.
    # The endpoint must never add an unpinned name-only required status.
    require(set(contexts) in (set(), {name for name, _ in expected}),
            "unapproved-legacy-context")
    return True


def main(argv):
    require(len(argv) == 3, "usage: validator PINS.json PROTECTION.json")
    pins = json.loads(Path(argv[1]).read_text(encoding="utf-8"))
    protection = (json.load(sys.stdin) if argv[2] == "-" else
                  json.loads(Path(argv[2]).read_text(encoding="utf-8")))
    verify(pins, protection)
    print("SOURCE_MAIN_REQUIRED_CHECK_APP_IDS=PASS")


if __name__ == "__main__":
    try:
        main(sys.argv)
    except (Denied, OSError, UnicodeError, json.JSONDecodeError) as error:
        print("SOURCE_MAIN_REQUIRED_CHECK_APP_IDS=DENIED:" + str(error), file=sys.stderr)
        raise SystemExit(1)
