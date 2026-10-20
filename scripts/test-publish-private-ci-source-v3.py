#!/usr/bin/env python3
"""Source-free unit tests for the LLVM private-CI v3 publisher."""
import base64
import datetime as dt
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("publish-private-ci-source-v3.py")
SPEC = importlib.util.spec_from_file_location("llvm_private_ci_v3", SCRIPT)
v3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v3)

SOURCE_TREE = "a" * 40
BASE_TREE = "b" * 40
CARRIER_COMMIT = "c" * 40
BASE = "d" * 40
WORKFLOW = "e" * 40
PLAN_BLOB = "f" * 40
PROTECTED = "1" * 40
DATA = b"synthetic llvm source\n"
BLOB = v3.git_blob_sha(DATA)
PLAN = (
    json.dumps(
        {
            "schema": "ores-private-ci-test-plan-v1",
            "source_repo": v3.SOURCE_REPO,
            "allowed_prs": [669],
            "commands": [
                {
                    "argv": ["python3", "-c", "print('ok')"],
                    "cwd": ".",
                    "timeout_seconds": 30,
                }
            ],
        },
        sort_keys=True,
        separators=(",", ":"),
    )
    + "\n"
).encode()


def carrier_contract():
    return {
        "repository_id": 1323598177,
        "base_sha": BASE,
        "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
        "workflow_blob_sha": WORKFLOW,
        "test_plan_path": "ci/test-plans/llvm-pr669.json",
        "test_plan_blob_sha": PLAN_BLOB,
    }


class LlvmPrivateCiV3Test(unittest.TestCase):
    def test_allowlist_is_exact_and_fail_closed(self):
        v3.parse_allowlist(b"# x\ntrusted/ci\n", "trusted/ci")
        for raw in (
            b"# empty\n",
            b"trusted/*\n",
            b"trusted/ci \n",
            b"trusted/ci\nTRUSTED/CI\n",
        ):
            with self.subTest(raw=raw), self.assertRaises(v3.Denied):
                v3.parse_allowlist(raw, "trusted/ci")

    def test_contract_pins_repo_base_workflow_plan_and_pr(self):
        raw = json.dumps(
            {
                "schema": 1,
                "carriers": {
                    "trusted/ci": {
                        "repository_id": 42,
                        "base_sha": BASE,
                        "workflow_path": ".github/workflows/oreslang-exact-tree-carrier.yml",
                        "workflow_blob_sha": WORKFLOW,
                        "test_plans": {
                            "ci/test-plans/llvm-pr669.json": {
                                "blob_sha": PLAN_BLOB,
                                "allowed_prs": [669],
                            }
                        },
                    }
                },
            }
        ).encode()
        got = v3.parse_contract(
            raw, "trusted/ci", 669, "ci/test-plans/llvm-pr669.json"
        )
        self.assertEqual(got["repository_id"], 42)
        self.assertEqual(got["base_sha"], BASE)
        self.assertEqual(got["workflow_blob_sha"], WORKFLOW)
        with self.assertRaisesRegex(v3.Denied, "not authorized"):
            v3.parse_contract(
                raw, "trusted/ci", 670, "ci/test-plans/llvm-pr669.json"
            )

    def test_carrier_main_requires_protection_and_codeowner_review(self):
        good_protection = {
            "required_pull_request_reviews": {
                "require_code_owner_reviews": True,
                "dismiss_stale_reviews": True,
                "required_approving_review_count": 1,
                "bypass_pull_request_allowances": {
                    "users": [], "teams": [], "apps": []
                },
            },
            "enforce_admins": {"enabled": True},
            "allow_force_pushes": {"enabled": False},
            "allow_deletions": {"enabled": False},
        }

        def good_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return good_protection
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=good_api):
            self.assertEqual(v3.checked_carrier_main("trusted/ci", BASE), BASE)

        def unprotected_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": False,
                    "commit": {"sha": BASE},
                }
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=unprotected_api):
            with self.assertRaisesRegex(v3.Denied, "carrier main is not protected"):
                v3.checked_carrier_main("trusted/ci", BASE)

        weak = json.loads(json.dumps(good_protection))
        weak["required_pull_request_reviews"]["require_code_owner_reviews"] = False

        def weak_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return weak
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=weak_api):
            with self.assertRaisesRegex(v3.Denied, "CODEOWNER"):
                v3.checked_carrier_main("trusted/ci", BASE)

    def test_carrier_main_rejects_bypass_or_base_drift(self):
        def api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": BASE},
                }
            if route == "repos/trusted/ci/branches/main/protection":
                return {
                    "required_pull_request_reviews": {
                        "require_code_owner_reviews": True,
                        "dismiss_stale_reviews": True,
                        "required_approving_review_count": 1,
                        "bypass_pull_request_allowances": {
                            "users": [{"login": "bypass"}],
                            "teams": [],
                            "apps": [],
                        },
                    },
                    "enforce_admins": {"enabled": True},
                    "allow_force_pushes": {"enabled": False},
                    "allow_deletions": {"enabled": False},
                }
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "bypasses"):
                v3.checked_carrier_main("trusted/ci", BASE)

        def drift_api(route, method="GET", payload=None):
            if route == "repos/trusted/ci/branches/main":
                return {
                    "name": "main",
                    "protected": True,
                    "commit": {"sha": "2" * 40},
                }
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=drift_api):
            with self.assertRaisesRegex(v3.Denied, "advanced beyond reviewed base"):
                v3.checked_carrier_main("trusted/ci", BASE)

    def test_exact_source_tree_reads_only_pinned_git_blobs(self):
        def api(route, method="GET", payload=None):
            if route == f"repos/{v3.SOURCE_REPO}/pulls/669":
                return {
                    "number": 669,
                    "state": "open",
                    "head": {
                        "sha": "9" * 40,
                        "repo": {"full_name": v3.SOURCE_REPO},
                    },
                    "base": {"ref": "main"},
                }
            if route == f"repos/{v3.SOURCE_REPO}/git/commits/" + "9" * 40:
                return {"sha": "9" * 40, "tree": {"sha": SOURCE_TREE}}
            if route == f"repos/{v3.SOURCE_REPO}/git/trees/{SOURCE_TREE}?recursive=1":
                return {
                    "sha": SOURCE_TREE,
                    "truncated": False,
                    "tree": [
                        {
                            "path": "src/synthetic.cpp",
                            "type": "blob",
                            "mode": "100644",
                            "sha": BLOB,
                        }
                    ],
                }
            if route == f"repos/{v3.SOURCE_REPO}/git/blobs/{BLOB}":
                return {
                    "sha": BLOB,
                    "encoding": "base64",
                    "content": base64.b64encode(DATA).decode(),
                }
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=api):
            tree, files, rows, digest = v3.exact_source_tree(669, "9" * 40)
        self.assertEqual(tree, SOURCE_TREE)
        self.assertEqual(files, [("src/synthetic.cpp", "100644", BLOB, DATA)])
        self.assertEqual(rows[0][:3], ["src/synthetic.cpp", "100644", BLOB])
        self.assertEqual(
            digest, hashlib.sha256(v3.canonical_bytes(rows)).hexdigest()
        )

    def test_source_tree_rejects_symlink_mode_before_blob_read(self):
        seen = []
        def api(route, method="GET", payload=None):
            seen.append(route)
            if route.endswith("/pulls/669"):
                return {
                    "number": 669,
                    "state": "open",
                    "head": {
                        "sha": "9" * 40,
                        "repo": {"full_name": v3.SOURCE_REPO},
                    },
                    "base": {"ref": "main"},
                }
            if "/git/commits/" in route:
                return {"sha": "9" * 40, "tree": {"sha": SOURCE_TREE}}
            if "/git/trees/" in route:
                return {
                    "sha": SOURCE_TREE,
                    "truncated": False,
                    "tree": [
                        {
                            "path": "link",
                            "type": "blob",
                            "mode": "120000",
                            "sha": "2" * 40,
                        }
                    ],
                }
            raise AssertionError(route)
        with patch.object(v3, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "symlink"):
                v3.exact_source_tree(669, "9" * 40)
        self.assertFalse(any("/git/blobs/" in route for route in seen))

    def test_publish_binds_receipt_tree_branch_and_authorization(self):
        contract = carrier_contract()
        rows = [["src/synthetic.cpp", "100644", BLOB, len(DATA)]]
        manifest = hashlib.sha256(v3.canonical_bytes(rows)).hexdigest()
        writes = []
        blob_bytes = []
        commit_payload = {}

        def api(route, method="GET", payload=None):
            writes.append((method, route, payload))
            if method == "POST" and route.endswith("/git/blobs"):
                data = base64.b64decode(payload["content"], validate=True)
                blob_bytes.append(data)
                return {"sha": v3.git_blob_sha(data)}
            if method == "POST" and route.endswith("/git/trees"):
                if payload.get("base_tree") is None:
                    return {"sha": SOURCE_TREE}
                return {"sha": "3" * 40}
            if method == "POST" and route.endswith("/git/commits"):
                commit_payload.update(payload)
                return {"sha": CARRIER_COMMIT}
            if method == "POST" and route.endswith("/git/refs"):
                return {"ref": payload["ref"], "object": {"sha": payload["sha"]}}
            raise AssertionError((method, route))

        with (
            patch.object(v3, "protected_authority", return_value=(PROTECTED, contract)),
            patch.object(v3, "checked_destination", return_value=(BASE_TREE, PLAN)),
            patch.object(
                v3,
                "exact_source_tree",
                return_value=(
                    SOURCE_TREE,
                    [("src/synthetic.cpp", "100644", BLOB, DATA)],
                    rows,
                    manifest,
                ),
            ),
            patch.object(v3, "recheck_destination_identity"),
            patch.object(v3, "gh_api", side_effect=api),
        ):
            result = v3.publish(
                "trusted/ci",
                669,
                "9" * 40,
                "ci/test-plans/llvm-pr669.json",
                now=dt.datetime(2026, 10, 20, tzinfo=dt.timezone.utc),
            )

        self.assertEqual(result["branch"], "ci/oreslang-" + SOURCE_TREE)
        self.assertEqual(
            result["authorization_ref"], "ci/authorized-" + CARRIER_COMMIT
        )
        receipt_raw = next(
            data for data in blob_bytes
            if b'"schema":"ores-private-ci-receipt-v3"' in data
        )
        receipt = json.loads(receipt_raw)
        self.assertEqual(receipt["expires_at"], "2026-10-21T00:00:00Z")
        self.assertEqual(receipt["source"]["repo_id"], v3.SOURCE_REPO_ID)
        self.assertEqual(receipt["source"]["pr"], 669)
        self.assertEqual(receipt["source"]["head_sha"], "9" * 40)
        self.assertEqual(receipt["source"]["root_tree_sha"], SOURCE_TREE)
        self.assertEqual(receipt["source"]["manifest_sha256"], manifest)
        self.assertEqual(receipt["destination"]["repo_id"], 1323598177)
        self.assertEqual(receipt["test_plan"]["path"], contract["test_plan_path"])
        self.assertEqual(
            receipt["test_plan"]["sha256"], hashlib.sha256(PLAN).hexdigest()
        )
        digest = v3.canonical_digest(receipt)
        self.assertIn(
            "Oreslang-CI-Receipt-SHA256: " + digest,
            commit_payload["message"].splitlines(),
        )
        refs = [
            payload["ref"]
            for method, route, payload in writes
            if method == "POST" and route.endswith("/git/refs")
        ]
        self.assertEqual(
            refs,
            [
                "refs/heads/ci/oreslang-" + SOURCE_TREE,
                "refs/heads/ci/authorized-" + CARRIER_COMMIT,
            ],
        )


if __name__ == "__main__":
    unittest.main(verbosity=2)
