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

    def test_review_attested_unprotected_main_accepts_exact_independent_review(self):
        main_sha = "2" * 40
        authority_head = "3" * 40

        def raw_for(path):
            if path == v3.POLICY_PATH:
                return b"trusted/ci\n"
            return (path + "\n").encode()

        def tree_entries():
            result = {}
            for path in v3.REVIEWED_GOVERNANCE_PATHS:
                raw = raw_for(path)
                result[path] = (
                    "100644",
                    "blob",
                    hashlib.sha1(
                        b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw
                    ).hexdigest(),
                )
            return result

        def api(route, method="GET", payload=None):
            if route == f"repos/{v3.SOURCE_REPO}":
                return {
                    "id": v3.SOURCE_REPO_ID,
                    "full_name": v3.SOURCE_REPO,
                    "private": True,
                }
            if route == f"repos/{v3.SOURCE_REPO}/branches/{v3.SOURCE_BRANCH}":
                return {
                    "name": v3.SOURCE_BRANCH,
                    "protected": False,
                    "commit": {"sha": main_sha},
                }
            if route == f"repos/{v3.SOURCE_REPO}/pulls/{v3.REVIEW_AUTHORITY_PR}":
                return {
                    "number": v3.REVIEW_AUTHORITY_PR,
                    "state": "closed",
                    "merged_at": "2026-10-20T01:00:00Z",
                    "user": {"id": 424242, "login": "independent-author"},
                    "head": {
                        "sha": authority_head,
                        "repo": {
                            "full_name": v3.SOURCE_REPO,
                            "id": v3.SOURCE_REPO_ID,
                        },
                    },
                    "base": {
                        "ref": v3.SOURCE_BRANCH,
                        "repo": {
                            "full_name": v3.SOURCE_REPO,
                            "id": v3.SOURCE_REPO_ID,
                        },
                    },
                }
            raise AssertionError(route)

        def remote(path, _ref, _limit):
            raw = raw_for(path)
            sha = hashlib.sha1(
                b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw
            ).hexdigest()
            return raw, sha

        reviews = [{
            "id": 7,
            "state": "APPROVED",
            "commit_id": authority_head,
            "submitted_at": "2026-10-20T00:59:00Z",
            "user": {
                "id": v3.REVIEW_AUTHORITY_USER_ID,
                "login": v3.REVIEW_AUTHORITY_LOGIN,
            },
        }]
        entries = tree_entries()
        with (
            patch.object(v3, "gh_api", side_effect=api),
            patch.object(v3, "gh_api_list", return_value=reviews),
            patch.object(v3, "governance_tree_entries", return_value=entries),
            patch.object(v3, "remote_source_file", side_effect=remote),
        ):
            self.assertEqual(
                v3.authorized_source_main("trusted/ci"),
                main_sha,
            )

    def test_review_attested_authority_denies_revocation_author_collision_and_mode_drift(self):
        main_sha = "2" * 40
        authority_head = "3" * 40

        def raw_for(path):
            if path == v3.POLICY_PATH:
                return b"trusted/ci\n"
            return (path + "\n").encode()

        def entries():
            out = {}
            for path in v3.REVIEWED_GOVERNANCE_PATHS:
                raw = raw_for(path)
                out[path] = (
                    "100644",
                    "blob",
                    hashlib.sha1(
                        b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw
                    ).hexdigest(),
                )
            return out

        def authority(author_id=424242):
            return {
                "number": v3.REVIEW_AUTHORITY_PR,
                "state": "closed",
                "merged_at": "2026-10-20T01:00:00Z",
                "user": {"id": author_id, "login": "author"},
                "head": {
                    "sha": authority_head,
                    "repo": {
                        "full_name": v3.SOURCE_REPO,
                        "id": v3.SOURCE_REPO_ID,
                    },
                },
                "base": {
                    "ref": v3.SOURCE_BRANCH,
                    "repo": {
                        "full_name": v3.SOURCE_REPO,
                        "id": v3.SOURCE_REPO_ID,
                    },
                },
            }

        def base_api(author_id=424242):
            def api(route, method="GET", payload=None):
                if route == f"repos/{v3.SOURCE_REPO}":
                    return {
                        "id": v3.SOURCE_REPO_ID,
                        "full_name": v3.SOURCE_REPO,
                        "private": True,
                    }
                if route == f"repos/{v3.SOURCE_REPO}/branches/{v3.SOURCE_BRANCH}":
                    return {
                        "name": v3.SOURCE_BRANCH,
                        "protected": False,
                        "commit": {"sha": main_sha},
                    }
                if route == f"repos/{v3.SOURCE_REPO}/pulls/{v3.REVIEW_AUTHORITY_PR}":
                    return authority(author_id)
                raise AssertionError(route)
            return api

        approved = {
            "id": 7,
            "state": "APPROVED",
            "commit_id": authority_head,
            "submitted_at": "2026-10-20T00:58:00Z",
            "user": {
                "id": v3.REVIEW_AUTHORITY_USER_ID,
                "login": v3.REVIEW_AUTHORITY_LOGIN,
            },
        }
        revoked = dict(approved)
        revoked.update({
            "id": 8,
            "state": "CHANGES_REQUESTED",
            "submitted_at": "2026-10-20T00:59:00Z",
        })
        with (
            patch.object(v3, "gh_api", side_effect=base_api()),
            patch.object(v3, "gh_api_list", return_value=[approved, revoked]),
        ):
            with self.assertRaisesRegex(v3.Denied, "stale, late, or not approved"):
                v3.authorized_source_main("trusted/ci")

        with patch.object(
                v3, "gh_api",
                side_effect=base_api(v3.REVIEW_AUTHORITY_USER_ID)):
            with self.assertRaisesRegex(v3.Denied, "independent merged"):
                v3.authorized_source_main("trusted/ci")

        stable = entries()
        changed = dict(stable)
        target = "scripts/run-in-private-ci-destination.sh"
        changed[target] = ("100755", "blob", changed[target][2])

        def remote(path, _ref, _limit):
            raw = raw_for(path)
            sha = hashlib.sha1(
                b"blob " + str(len(raw)).encode("ascii") + b"\0" + raw
            ).hexdigest()
            return raw, sha

        with (
            patch.object(v3, "gh_api", side_effect=base_api()),
            patch.object(v3, "gh_api_list", return_value=[approved]),
            patch.object(v3, "governance_tree_entries", side_effect=[stable, changed]),
            patch.object(v3, "remote_source_file", side_effect=remote),
        ):
            with self.assertRaisesRegex(v3.Denied, "Git entry drifted"):
                v3.authorized_source_main("trusted/ci")

    def test_review_history_and_governance_tree_fail_closed(self):
        review = {
            "id": 7,
            "state": "APPROVED",
            "commit_id": "3" * 40,
            "submitted_at": "2026-10-20T00:00:00Z",
            "user": {
                "id": v3.REVIEW_AUTHORITY_USER_ID,
                "login": v3.REVIEW_AUTHORITY_LOGIN,
            },
        }
        with patch.object(v3, "gh_api_list", return_value=[review, dict(review)]):
            with self.assertRaisesRegex(v3.Denied, "invalid IDs"):
                v3.latest_authority_review()

        malformed = dict(review)
        malformed["submitted_at"] = "not-time"
        with patch.object(v3, "gh_api_list", return_value=[malformed]):
            with self.assertRaisesRegex(v3.Denied, "timestamp invalid"):
                v3.latest_authority_review()

        commit_sha = "4" * 40
        tree_sha = "5" * 40
        listing = [{
            "path": path,
            "mode": "100644",
            "type": "blob",
            "sha": "6" * 40,
        } for path in v3.REVIEWED_GOVERNANCE_PATHS]

        def api(route, method="GET", payload=None):
            if route.endswith("/git/commits/" + commit_sha):
                return {"sha": commit_sha, "tree": {"sha": tree_sha}}
            if route.endswith("/git/trees/" + tree_sha + "?recursive=1"):
                return {"sha": tree_sha, "truncated": False, "tree": listing}
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=api):
            self.assertEqual(
                set(v3.governance_tree_entries(commit_sha)),
                set(v3.REVIEWED_GOVERNANCE_PATHS),
            )

        unsafe = [dict(item) for item in listing]
        unsafe[0]["mode"] = "120000"

        def unsafe_api(route, method="GET", payload=None):
            if route.endswith("/git/commits/" + commit_sha):
                return {"sha": commit_sha, "tree": {"sha": tree_sha}}
            if route.endswith("/git/trees/" + tree_sha + "?recursive=1"):
                return {"sha": tree_sha, "truncated": False, "tree": unsafe}
            raise AssertionError(route)

        with patch.object(v3, "gh_api", side_effect=unsafe_api):
            with self.assertRaisesRegex(v3.Denied, "unsafe Git type/mode"):
                v3.governance_tree_entries(commit_sha)

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
