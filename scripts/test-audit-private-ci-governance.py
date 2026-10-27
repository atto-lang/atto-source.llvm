#!/usr/bin/env python3
"""Source-free offline tests of LLVM CI governance preflight."""
import importlib.util
import json
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
            "visibility": "private",
            "default_branch": "main",
            "fork": False,
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
                "require_last_push_approval": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [],
                    "teams": [],
                    "apps": [],
                },
            },
            "required_status_checks": {
                "strict": True,
                "contexts": [audit.SOURCE_POLICY_CHECK],
                "checks": [{
                    "context": audit.SOURCE_POLICY_CHECK,
                    "app_id": audit.GITHUB_ACTIONS_APP_ID,
                }],
            },
            "required_conversation_resolution": {"enabled": True},
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
            (dict(repo, visibility="public"), branch, protection),
            ({k: v for k, v in repo.items() if k != "visibility"}, branch, protection),
            (dict(repo, fork=True), branch, protection),
            ({k: v for k, v in repo.items() if k != "fork"}, branch, protection),
            (dict(repo, archived=True), branch, protection),
            (dict(repo, archived="false"), branch, protection),
            ({k: v for k, v in repo.items() if k != "archived"}, branch, protection),
            (dict(repo, disabled=True), branch, protection),
            (dict(repo, disabled=0), branch, protection),
            ({k: v for k, v in repo.items() if k != "disabled"}, branch, protection),
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
        # API responses and future review-bypass capabilities must fail
        # closed, not be coerced into seemingly safe empty objects/lists.
        invalids.extend([
            (None, branch, protection),
            ([], branch, protection),
            (repo, None, protection),
            (repo, [], protection),
            (repo, branch, None),
            (repo, branch, []),
            (repo, {**branch, "commit": {"sha": None}}, protection),
            (repo, {**branch, "commit": {"sha": True}}, protection),
            (repo, branch, {
                **protection,
                "required_pull_request_reviews": {
                    **reviews,
                    "bypass_pull_request_allowances": {
                        "users": [], "teams": [], "apps": [],
                        "delegated_roles": [],
                    },
                },
            }),
            (repo, branch, {
                **protection,
                "required_pull_request_reviews": {
                    **reviews,
                    "bypass_pull_request_allowances": {
                        "users": None, "teams": [], "apps": [],
                    },
                },
            }),
        ])
        invalids.extend([
            (
                repo,
                branch,
                {
                    **protection,
                    "required_pull_request_reviews": {
                        **reviews,
                        "require_last_push_approval": False,
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_conversation_resolution": {"enabled": False},
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_status_checks": {
                        **protection["required_status_checks"],
                        "strict": False,
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_status_checks": {
                        "strict": True,
                        "contexts": [audit.SOURCE_POLICY_CHECK],
                        "checks": [{
                            "context": audit.SOURCE_POLICY_CHECK,
                            "app_id": 999999,
                        }],
                    },
                },
            ),
            (
                repo,
                branch,
                {
                    **protection,
                    "required_status_checks": {
                        "strict": True,
                        "contexts": [],
                        "checks": [{
                            "context": "unrelated-check",
                            "app_id": audit.GITHUB_ACTIONS_APP_ID,
                        }],
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
                            "users": [], "teams": [],
                        },
                    },
                },
            ),
        ])

        for r, b, p in invalids:
            with self.subTest(input=(r, b, p)), self.assertRaises(audit.GovernanceError):
                audit.check_live_metadata(r, b, p)

    def test_approved_destination_requires_exact_reviewed_contract(self):
        approved = ("private-org/ci-test",)
        valid = {
            "schema": 1,
            "carriers": {
                "private-org/ci-test": {
                    "repository_id": 42,
                    "base_sha": "a" * 40,
                    "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
                    "workflow_blob_sha": "b" * 40,
                    "review_pr": 17,
                    "test_plans": {
                        "ci/test-plans/llvm-pr970.json": {
                            "blob_sha": "c" * 40,
                            "allowed_prs": [970],
                        }
                    },
                }
            },
        }
        audit.check_contracts_for_approved(json.dumps(valid), approved)

        invalids = []
        missing = json.loads(json.dumps(valid))
        del missing["carriers"]["private-org/ci-test"]["review_pr"]
        invalids.append(missing)
        zero_review = json.loads(json.dumps(valid))
        zero_review["carriers"]["private-org/ci-test"]["review_pr"] = 0
        invalids.append(zero_review)
        bool_review = json.loads(json.dumps(valid))
        bool_review["carriers"]["private-org/ci-test"]["review_pr"] = True
        invalids.append(bool_review)
        bad_sha = json.loads(json.dumps(valid))
        bad_sha["carriers"]["private-org/ci-test"]["base_sha"] = "not-a-sha"
        invalids.append(bad_sha)
        bad_path = json.loads(json.dumps(valid))
        bad_path["carriers"]["private-org/ci-test"]["workflow_path"] = "../workflow.yml"
        invalids.append(bad_path)
        duplicate_pr = json.loads(json.dumps(valid))
        duplicate_pr["carriers"]["private-org/ci-test"]["test_plans"][
            "ci/test-plans/llvm-pr970.json"
        ]["allowed_prs"] = [970, 970]
        invalids.append(duplicate_pr)
        absent = {"schema": 1, "carriers": {}}
        invalids.append(absent)

        for doc in invalids:
            with self.subTest(doc=doc), self.assertRaises(audit.GovernanceError):
                audit.check_contracts_for_approved(json.dumps(doc), approved)

    def test_carrier_contract_json_rejects_duplicate_keys_and_nonfinite(self):
        with self.assertRaisesRegex(audit.GovernanceError, "duplicate"):
            audit.check_contracts_for_approved(
                '{"schema":1,"schema":1,"carriers":{}}',
                (),
            )
        with self.assertRaisesRegex(audit.GovernanceError, "nonfinite"):
            audit.check_contracts_for_approved(
                '{"schema":1,"carriers":{},"x":NaN}',
                (),
            )

    def test_missing_codeowner_rule_fails(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for relative in (
                audit.APPROVAL_PATH,
                audit.CONTRACT_PATH,
                audit.OWNERS_PATH,
                audit.DOC_PATH,
            ):
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
            (root / audit.APPROVAL_PATH).write_text("# empty\n")
            (root / audit.CONTRACT_PATH).write_text('{"schema":1,"carriers":{}}\n')
            (root / audit.OWNERS_PATH).write_text("/.github/CODEOWNERS @ORESoftware\n")
            (root / audit.DOC_PATH).write_text("protected private tree canary Direct GitHub connector review-attested")
            with self.assertRaises(audit.GovernanceError):
                audit.check_policy_files(root)


if __name__ == "__main__":
    unittest.main(verbosity=2)
