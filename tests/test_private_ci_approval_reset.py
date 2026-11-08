"""Bootstrap denial canary: verifies zero active cross-org private-source grants.

This is a local, source-only policy regression suite, NOT an authentication
of protected main, reviewer approval, or GitHub Actions allocation.
"""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
POLICIES = #[".github/private-ci-approved-repositories.txt"]#
FORMER_CANDIDATES = #["streempilot-test/browser-studio-e2e"]#
PROFILE = 'llvm'


def read_regular_policy(path):
    if path.is_symlink() or path.parent.is_symlink() or not path.is_file():
        raise ValueError("policy must be a regular, non-symlink file")
    raw = path.read_bytes()
    return validate_policy_bytes(raw)


def validate_policy_bytes(raw):
    if not isinstance(raw, bytes) or not raw or len(raw) > 65536:
        raise ValueError("policy bytes missing or oversized")
    if any(c != 10 and not 32 <= c <= 126 for c in raw):
        raise ValueError("noncanonical policy bytes")
    if not raw.endswith(b"\n"):
        raise ValueError("policy requires LF terminator")
    lines = raw.decode("ascii").split("\n")
    active = []
    for line in lines:
        if not line:
            continue
        if line != line.strip():
            raise ValueError("ambiguous whitespace")
        if line.startswith("#"):
            continue
        active.append(line)
    return active


def require_zero_authorizations(raw):
    if validate_policy_bytes(raw):
        raise ValueError("active private-source transfer approval is forbidden in bootstrap")


class PrivateCiApprovalResetTest(unittest.TestCase):
    def test_active_destinations_and_snapshots_are_empty(self):
        for name in POLICIES:
            with self.subTest(name=name):
                path = ROOT / name
                self.assertEqual(read_regular_policy(path), [])

    def test_superseded_candidates_removed_from_live_policy_bytes(self):
        for name in POLICIES:
            raw = (ROOT / name).read_bytes()
            for candidate in FORMER_CANDIDATES:
                with self.subTest(policy=name, candidate=candidate):
                    self.assertNotIn(candidate.encode("ascii"), raw)

    def test_malformed_inputs_and_accidental_grants_fail_closed(self):
        original = b"# Private CI approvals disabled\n"
        self.assertIsNone(require_zero_authorizations(original))
        attacks = [
            b"",
            b"# Missing final newline",
            b"# comment\r\n",
            b"# comment\x00\n",
            b"# comment\xff\n",
            b"  # indented comment\n",
            b"# trailing whitespace \n",
            b"\t# lookalike\n",
            b"owner/private-ci 123456\n",
            b"# comment\nowner/private-ci 123456\n",
            b"# comment\nowner/private-ci\n",
            b"# comment\n" + b"A" * 65536,
        ]
        for index, raw in enumerate(attacks):
            with self.subTest(attack=index):
                with self.assertRaises(ValueError):
                    require_zero_authorizations(raw)

    def test_policy_parent_and_file_are_not_symlinks(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            policy = root / "policy"
            policy.write_bytes(b"# deny\n")
            link = root / "linked"
            link.symlink_to(policy)
            with self.assertRaises(ValueError):
                read_regular_policy(link)

    def test_real_v3_publisher_parser_denies_old_destination(self):
        script = ROOT / "scripts/publish-private-ci-source-v3.py"
        self.assertTrue(script.is_file() and not script.is_symlink())
        module_spec = importlib.util.spec_from_file_location("checked_llvm_v3", script)
        module = importlib.util.module_from_spec(module_spec)
        module_spec.loader.exec_module(module)
        raw = (ROOT / POLICIES[0]).read_bytes()
        with self.assertRaises(module.Denied):
            module.parse_allowlist(raw, "streempilot-test/browser-studio-e2e")
        with self.assertRaises(module.Denied):
            module.parse_allowlist(raw, "sonus-auris/sonus-auris.infra")
        self.assertIsNone(module.parse_allowlist(
            b"# historic proposal only\nowner/name\n",
            "owner/name"))
        with self.assertRaises(module.Denied):
            module.parse_allowlist(
                b"# proposal\nowner/name\nowner/name\n",
                "owner/name")



if __name__ == "__main__":
    unittest.main()
