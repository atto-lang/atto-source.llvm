#!/usr/bin/env python3
"""Fail-closed allowlist preflight for cross-organization private-source CI.

This checks *approval only*. The caller must separately verify the current
GitHub repository is private, has appropriate owner/security consent, and has
actual available Actions capacity. It never copies files or contacts GitHub.
"""

import argparse
from pathlib import Path
import re
import sys

_REPOSITORY = re.compile(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+\Z")


class MirrorPolicyError(ValueError):
    """Invalid policy or unauthorized destination."""


def normalize_repository(value: str) -> str:
    if not isinstance(value, str) or value != value.strip() or not _REPOSITORY.fullmatch(value):
        raise MirrorPolicyError("repository must be an exact owner/repo without whitespace or wildcards")
    if any(part in {".", ".."} for part in value.split("/")):
        raise MirrorPolicyError("repository owner and name must not be path navigation components")
    return value.lower()


def read_approved_repositories(policy_file: Path) -> set[str]:
    try:
        # A caller-controlled symlink (or special file such as a FIFO) can
        # redirect/block an otherwise pinned policy pathname. Reject both
        # before reading, and bound the approval manifest to 16 KiB.
        if policy_file.is_symlink():
            raise MirrorPolicyError("private CI allowlist must be a regular non-symlink file")
        if not policy_file.exists():
            raise MirrorPolicyError("cannot read the reviewed private CI allowlist")
        if not policy_file.is_file():
            raise MirrorPolicyError("private CI allowlist must be a regular non-symlink file")
        if policy_file.stat().st_size > 16 * 1024:
            raise MirrorPolicyError("private CI allowlist exceeds 16 KiB safety limit")
        policy = policy_file.read_text(encoding="utf-8")
    except (OSError, UnicodeError) as exc:
        raise MirrorPolicyError("cannot read the reviewed private CI allowlist") from exc

    approved: set[str] = set()
    for line_number, line in enumerate(policy.splitlines(), 1):
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        try:
            destination = normalize_repository(line)
        except MirrorPolicyError as exc:
            raise MirrorPolicyError(f"invalid allowlist entry on line {line_number}: {exc}") from exc
        if destination in approved:
            raise MirrorPolicyError(f"duplicate allowlist entry on line {line_number}")
        approved.add(destination)
    return approved


def assert_destination_approved(source: str, destination: str, policy_file: Path) -> None:
    original = normalize_repository(source)
    target = normalize_repository(destination)
    if original == target:
        raise MirrorPolicyError("cross-organization mirror must name a different repository")
    if original.split("/", 1)[0] == target.split("/", 1)[0]:
        raise MirrorPolicyError("cross-organization mirror requires different organization owners")
    if target not in read_approved_repositories(policy_file):
        raise MirrorPolicyError("destination is not approved in the protected private CI allowlist")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, help="Source owner/repo")
    parser.add_argument("--destination", required=True, help="Destination owner/repo")
    # Deliberately no --allowlist option: allowing an alternate unreviewed
    # path would defeat protected-branch review of the source repository's
    # single security-controlled destination manifest.
    args = parser.parse_args(argv)
    reviewed_allowlist = (
        Path(__file__).resolve().parents[1]
        / ".github/private-ci-approved-repositories.txt"
    )
    try:
        assert_destination_approved(args.source, args.destination, reviewed_allowlist)
    except MirrorPolicyError as exc:
        print(f"PRIVATE CI MIRROR DENIED: {exc}", file=sys.stderr)
        return 1
    print("Allowlist preflight passed; PRIVACY, ACCESS, SECURITY REVIEW AND ACTIONS CAPACITY remain to be verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
