"""Source-free negative acceptance for a private CI transfer authorization gate."""
import json
import tempfile
import unittest
from pathlib import Path

from private_ci_export_gate import GateRejected, authorize, main

A = "a" * 40
B = "b" * 40
C = "c" * 40
DEST = "approved-test/private-runner"
ARGS = [DEST, 1234, A, ".github/workflows/carrier.yml", B,
        "ci/test-plans/llvm-pr1123.json", C, 1123]


class ExportGateTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / ".github").mkdir()
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            "# protected main approval\n" + DEST + "\n", encoding="utf-8")
        self.data = {"schema": 1, "carriers": {DEST: {
            "repository_id": 1234, "base_sha": A,
            "workflow_path": ".github/workflows/carrier.yml", "workflow_blob_sha": B,
            "test_plans": {"ci/test-plans/llvm-pr1123.json": {
                "blob_sha": C, "allowed_prs": [1123]}}}}}
        self.write_contract()

    def write_contract(self):
        (self.root / ".github/private-ci-carrier-contracts.json").write_text(
            json.dumps(self.data), encoding="utf-8")

    def check_rejected(self, *args):
        with self.assertRaises(GateRejected):
            authorize(self.root, *(args or ARGS))

    def test_positive_exact_pins(self):
        authorize(self.root, *ARGS)

    def test_default_deny(self):
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            "# none approved\n", encoding="utf-8")
        self.check_rejected()

    def test_wrong_repo_id(self):
        self.check_rejected(DEST, 1235, A, ARGS[3], B, ARGS[5], C, 1123)

    def test_wrong_base(self):
        self.check_rejected(DEST, 1234, B, ARGS[3], B, ARGS[5], C, 1123)

    def test_wrong_workflow(self):
        self.check_rejected(DEST, 1234, A, ARGS[3], C, ARGS[5], C, 1123)

    def test_unlisted_pr(self):
        self.check_rejected(DEST, 1234, A, ARGS[3], B, ARGS[5], C, 1124)

    def test_wrong_test_plan(self):
        self.check_rejected(DEST, 1234, A, ARGS[3], B, ARGS[5], B, 1123)

    def test_duplicate_json_keys(self):
        (self.root / ".github/private-ci-carrier-contracts.json").write_text(
            '{"schema":1,"schema":1,"carriers":{}}', encoding="utf-8")
        self.check_rejected()

    def test_allowlist_duplicate(self):
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            DEST + "\n" + DEST + "\n", encoding="utf-8")
        self.check_rejected()

    def test_allowlist_bad_whitespace(self):
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            DEST + " \n", encoding="utf-8")
        self.check_rejected()

    def test_policy_symlink(self):
        file = self.root / ".github/private-ci-approved-repositories.txt"
        file.unlink()
        file.symlink_to("private-ci-carrier-contracts.json")
        self.check_rejected()

    def test_path_traversal(self):
        self.check_rejected(DEST, 1234, A, "../bad.yml", B, ARGS[5], C, 1123)

    def test_invalid_sha(self):
        self.check_rejected(DEST, 1234, A.upper(), ARGS[3], B, ARGS[5], C, 1123)

    def test_cli_denies_unapproved(self):
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            "# none approved\n", encoding="utf-8")
        self.assertEqual(main(["--policy-root", str(self.root), "--destination", DEST,
                               "--repository-id", "1234", "--base-sha", A,
                               "--workflow-path", ARGS[3], "--workflow-blob-sha", B,
                               "--plan-path", ARGS[5], "--plan-blob-sha", C,
                               "--pr", "1123"]), 1)


    def test_nonobject_policy_rejected(self):
        (self.root / ".github/private-ci-carrier-contracts.json").write_text(
            "[]", encoding="utf-8")
        self.check_rejected()

    def test_boolean_schema_rejected(self):
        self.data["schema"] = True  # bool == 1 in Python, but never a schema version
        self.write_contract()
        self.check_rejected()

    def test_duplicate_allowed_pr_rejected(self):
        self.data["carriers"][DEST]["test_plans"][ARGS[5]]["allowed_prs"] = [1123, 1123]
        self.write_contract()
        self.check_rejected()

    def test_nonfinite_json_rejected(self):
        self.data["unexpected"] = float("nan")
        self.write_contract()
        self.check_rejected()

    def test_control_character_path_rejected(self):
        self.check_rejected(DEST, 1234, A, ".github/workflows/carrier.yml\nextra",
                            B, ARGS[5], C, 1123)

    def test_oversized_policy_file_rejected(self):
        (self.root / ".github/private-ci-approved-repositories.txt").write_text(
            "#" + ("x" * (1024 * 1024)) + "\n" + DEST + "\n",
            encoding="utf-8")
        self.check_rejected()


if __name__ == "__main__":
    unittest.main()
