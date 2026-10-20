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

    def test_reject_unprotected_and_missing_codeowner_review(self):
        repo = {"full_name": audit.SOURCE, "private": True, "default_branch": "main"}
        protected = {"name": "main", "protected": True}
        protection = {
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True, "dismiss_stale_reviews": True
            },
            "enforce_admins": {"enabled": True}
        }
        audit.check_live_metadata(repo, protected, protection)
        invalids = [
            (dict(repo, private=False), protected, protection),
            (repo, dict(protected, protected=False), protection),
            (repo, protected, {**protection, "enforce_admins": {"enabled": False}}),
            (repo, protected, {**protection, "required_pull_request_reviews": {"require_code_owner_reviews": False}}),
            (repo, protected, {**protection, "required_pull_request_reviews": {"require_code_owner_reviews": True, "dismiss_stale_reviews": False}}),
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
            (root / audit.DOC_PATH).write_text("protected private tree canary Direct GitHub connector")
            with self.assertRaises(audit.GovernanceError):
                audit.check_policy_files(root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
