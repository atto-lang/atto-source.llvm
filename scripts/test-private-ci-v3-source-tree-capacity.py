#!/usr/bin/env python3
"""Source-free negative/positive tests for proposed V3 exact-tree file cap.

Synthetic data only. No private tree, no GitHub credentials or API requests.
This test never grants private-source export authority.
"""
import base64
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).with_name("publish-private-ci-source-v3.py")
SPEC = importlib.util.spec_from_file_location("llvm_v3_capacity_review", SCRIPT)
v3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v3)

HEAD = "9" * 40
TREE = "a" * 40
PAYLOAD = b"synthetic file-count gate only\n"
BLOB = v3.git_blob_sha(PAYLOAD)


def synthetic_api(count, *, declared_size=None, base="main"):
    calls = []
    entries = [
        {
            "path": "tests/synthetic/file_%04d.txt" % i,
            "type": "blob",
            "mode": "100644",
            "sha": BLOB,
            "size": len(PAYLOAD) if declared_size is None else declared_size,
        }
        for i in range(count)
    ]

    def api(route, method="GET", payload=None):
        calls.append(route)
        if method != "GET":
            raise AssertionError("unexpected write attempt")
        if route == "repos/%s/pulls/1747" % v3.SOURCE_REPO:
            return {
                "number": 1747,
                "state": "open",
                "head": {
                    "sha": HEAD,
                    "repo": {"full_name": v3.SOURCE_REPO},
                },
                "base": {"ref": base},
            }
        if route == "repos/%s/git/commits/%s" % (v3.SOURCE_REPO, HEAD):
            return {"sha": HEAD, "tree": {"sha": TREE}}
        if route == "repos/%s/git/trees/%s?recursive=1" % (v3.SOURCE_REPO, TREE):
            return {"sha": TREE, "truncated": False, "tree": entries}
        if route == "repos/%s/git/blobs/%s" % (v3.SOURCE_REPO, BLOB):
            return {
                "sha": BLOB,
                "encoding": "base64",
                "content": base64.b64encode(PAYLOAD).decode("ascii"),
            }
        raise AssertionError("unexpected API path: " + route)

    return api, calls


class ReviewOnlyCapacityTest(unittest.TestCase):
    def test_1829_file_pr_head_under_2048_cap_is_admissible(self):
        self.assertEqual(v3.MAX_FILES, 2048)
        self.assertEqual(v3.MAX_TOTAL, 40 * 1024 * 1024)
        self.assertEqual(v3.MAX_FILE, 10 * 1024 * 1024)
        api, calls = synthetic_api(1829)
        with patch.object(v3, "gh_api", side_effect=api):
            root, files, rows, digest = v3.exact_source_tree(1747, HEAD)
        self.assertEqual(root, TREE)
        self.assertEqual(len(files), 1829)
        self.assertEqual(len(rows), 1829)
        self.assertEqual(len(digest), 64)
        self.assertEqual(sum("/git/blobs/" in path for path in calls), 1829)

    def test_exact_2048_file_boundary_is_admissible(self):
        api, calls = synthetic_api(2048)
        with patch.object(v3, "gh_api", side_effect=api):
            _root, files, _rows, _digest = v3.exact_source_tree(1747, HEAD)
        self.assertEqual(len(files), v3.MAX_FILES)
        self.assertEqual(sum("/git/blobs/" in path for path in calls), v3.MAX_FILES)

    def test_2049_files_fail_before_any_blob_read(self):
        api, calls = synthetic_api(2049)
        with patch.object(v3, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "bounded capacity"):
                v3.exact_source_tree(1747, HEAD)
        self.assertFalse(any("/git/blobs/" in path for path in calls))

    def test_40_mib_aggregate_cap_still_fails_before_blob_read(self):
        api, calls = synthetic_api(1829, declared_size=24000)
        with patch.object(v3, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "bounded capacity"):
                v3.exact_source_tree(1747, HEAD)
        self.assertFalse(any("/git/blobs/" in path for path in calls))

    def test_stacked_pr_is_not_authorized_before_source_tree_read(self):
        api, calls = synthetic_api(
            1829, base="feat/shared-immutable-payload-host-20261112"
        )
        with patch.object(v3, "gh_api", side_effect=api):
            with self.assertRaisesRegex(v3.Denied, "not exact open PR head against main"):
                v3.exact_source_tree(1747, HEAD)
        self.assertEqual(calls, ["repos/%s/pulls/1747" % v3.SOURCE_REPO])


    def test_truncated_github_tree_is_denied_before_any_blob_read(self):
        api, calls = synthetic_api(1829)
        def truncated(route, method="GET", payload=None):
            response = api(route, method=method, payload=payload)
            if "/git/trees/" in route:
                return dict(response, truncated=True)
            return response
        with patch.object(v3, "gh_api", side_effect=truncated):
            with self.assertRaisesRegex(v3.Denied, "tree incomplete"):
                v3.exact_source_tree(1747, HEAD)
        self.assertFalse(any("/git/blobs/" in path for path in calls))

    def test_symlink_mode_is_denied_before_any_blob_read(self):
        api, calls = synthetic_api(1829)
        def symlink(route, method="GET", payload=None):
            response = api(route, method=method, payload=payload)
            if "/git/trees/" in route:
                tree = dict(response)
                tree["tree"] = [dict(response["tree"][0], mode="120000")] + response["tree"][1:]
                return tree
            return response
        with patch.object(v3, "gh_api", side_effect=symlink):
            with self.assertRaisesRegex(v3.Denied, "symlink"):
                v3.exact_source_tree(1747, HEAD)
        self.assertFalse(any("/git/blobs/" in path for path in calls))

    def test_oversize_file_denied_before_any_blob_read(self):
        api, calls = synthetic_api(1829)
        def oversized(route, method="GET", payload=None):
            response = api(route, method=method, payload=payload)
            if "/git/trees/" in route:
                tree = dict(response)
                tree["tree"] = [dict(response["tree"][0], size=v3.MAX_FILE+1)] + response["tree"][1:]
                return tree
            return response
        with patch.object(v3, "gh_api", side_effect=oversized):
            with self.assertRaisesRegex(v3.Denied, "file exceeds"):
                v3.exact_source_tree(1747, HEAD)
        self.assertFalse(any("/git/blobs/" in path for path in calls))


if __name__ == "__main__":
    unittest.main()
