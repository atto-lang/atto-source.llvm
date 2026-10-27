#!/usr/bin/env python3
"""Source-free offline tests of LLVM CI governance preflight."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("audit-private-ci-governance.py")
spec = importlib.util.spec_from_file_location("audit_policy", SCRIPT)
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class PolicyAuditTest(unittest.TestCase):
    def test_empty_allowlist(self):
        self.assertEqual((), audit.parse_allowlist("# None approved\n"))

    def test_valid_exact_repo(self):
        self.assertEqual(("private-org/ci-test",), audit.parse_allowlist("private-org/ci-test\n"))

    def test_forbid_invalid_lines_and_duplicates(self):
        cases = [
            "x/y ", " x/y", "x/*", "x/y # comment", "x/y\nx/y",
            "X/Y\nx/y", "ores-truffle-oreslang/oreslang-source.llvm",
            "https://github.com/x/y", "x/../y", "x/..", "x/.",
        ]
        for case in cases:
            with self.subTest(case=case), self.assertRaises(audit.GovernanceError):
                audit.parse_allowlist(case)

    def _valid_live_metadata(self):
        repo = {
            "full_name": audit.SOURCE,
            "id": audit.SOURCE_ID,
            "private": True,
            "default_branch": "main",
            "archived": False,
            "disabled": False,
        }
        branch = {
            "name": "main",
            "protected": True,
            "commit": {"sha": "a" * 40},
        }
        protection = {
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [],
                    "teams": [],
                    "apps": [],
                },
            },
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
        }
        return repo, branch, protection

    def test_accepts_strict_protected_main_metadata(self):
        audit.check_live_metadata(*self._valid_live_metadata())

    def test_rejects_weak_or_ambiguous_protection_metadata(self):
        repo, branch, protection = self._valid_live_metadata()
        reviews = protection["required_pull_request_reviews"]
        invalids = [
            (dict(repo, id=audit.SOURCE_ID + 1), branch, protection),
            (dict(repo, private=False), branch, protection),
            (dict(repo, archived=True), branch, protection),
            (dict(repo, disabled=True), branch, protection),
            (repo, dict(branch, protected=False), protection),
            (repo, {**branch, "commit": {"sha": "not-a-sha"}}, protection),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "required_approving_review_count": 0,
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "required_approving_review_count": True,
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "bypass_pull_request_allowances": {
                            "users": [{"login": "bypass"}],
                            "teams": [],
                            "apps": [],
                        },
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "bypass_pull_request_allowances": None,
                    },
                },
            ),
            (
                repo,
                branch,
                {**protection, "enforce_admins": {"enabled": False}},
            ),
            (
                repo,
                branch,
                {**protection, "allow_force_pushes": {"enabled": True}},
            ),
            (
                repo,
                branch,
                {**protection, "allow_force_pushes": None},
            ),
            (
                repo,
                branch,
                {**protection, "allow_deletions": {"enabled": True}},
            ),
            (
                repo,
                branch,
                {**protection, "allow_deletions": None},
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "require_code_owner_reviews": False,
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "dismiss_stale_reviews": False,
                    },
                },
            ),
        ]
        for r, b, p in invalids:
            with self.subTest(input=(r, b, p)), self.assertRaises(audit.GovernanceError):
                audit.check_live_metadata(r, b, p)

    def test_missing_codeowner_rule_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for relative in (audit.APPROVAL_PATH, audit.OWNERS_PATH, audit.DOC_PATH):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / audit.APPROVAL_PATH).write_text("# empty\n")
            (root / audit.OWNERS_PATH).write_text("/.github/CODEOWNERS @ORESoftware\n")
            (root / audit.DOC_PATH).write_text("protected private tree canary Direct GitHub connector review-attested")
            with self.assertRaises(audit.GovernanceError):
                audit.check_policy_files(root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
