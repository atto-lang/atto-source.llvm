#!/usr/bin/env python3
"""Synthetic no-GitHub no-private-source exact check identity negative tests."""
import copy
import importlib.util
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

MODULE = Path(__file__).with_name("verify_llvm_required_check_pins.py")
PENDING = Path(__file__).with_name("llvm_required_check_pins.json")
spec = importlib.util.spec_from_file_location("pin_checker", MODULE)
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)


def fixture():
    expected = {
        "version": 1,
        "repository": checker.REPOSITORY,
        "branch": "main",
        "state": "APPROVED",  # Synthetic fixture, not an actual approval.
        "checks": [{"context": "synthetic/llvm-security", "app_id": 12345},
                   {"context": "synthetic/llvm-compiler", "app_id": 67890}],
    }
    observed = {
        "required_status_checks": {
            "strict": True,
            "checks": copy.deepcopy(expected["checks"]),
            "contexts": [i["context"] for i in expected["checks"]],
        }
    }
    return expected, observed


class PinningTests(unittest.TestCase):
    def test_synthetic_positive_and_order_independence(self):
        p, o = fixture()
        self.assertTrue(checker.verify(p, o))
        o["required_status_checks"]["checks"].reverse()
        self.assertTrue(checker.verify(p, o))
        o["required_status_checks"]["contexts"] = []
        self.assertTrue(checker.verify(p, o))

    def test_real_committed_policy_must_be_unapproved(self):
        pending = json.loads(PENDING.read_text())
        p, o = fixture()
        self.assertEqual(pending["state"], "UNAPPROVED")
        self.assertEqual(pending["checks"], [])
        with self.assertRaises(checker.Denied):
            checker.verify(pending, o)

    def test_reject_mutated_observation(self):
        scenarios = {
            "other_app": lambda x: x["required_status_checks"]["checks"][0].__setitem__("app_id", 67891),
            "other_context": lambda x: x["required_status_checks"]["checks"][0].__setitem__("context", "synthetic/impostor"),
            "removed": lambda x: x["required_status_checks"]["checks"].pop(),
            "extra": lambda x: x["required_status_checks"]["checks"].append({"context": "synthetic/extra", "app_id": 77}),
            "duplicate": lambda x: x["required_status_checks"]["checks"].append(copy.deepcopy(x["required_status_checks"]["checks"][0])),
            "duplicate_context": lambda x: x["required_status_checks"]["checks"].append({"context": "synthetic/llvm-compiler", "app_id": 77}),
            "empty": lambda x: x["required_status_checks"].__setitem__("checks", []),
            "name_only": lambda x: x["required_status_checks"].__setitem__("checks", [{"context":"synthetic/llvm-security", "app_id":None}]),
            "boolean_app": lambda x: x["required_status_checks"]["checks"][0].__setitem__("app_id", True),
            "zero_app": lambda x: x["required_status_checks"]["checks"][0].__setitem__("app_id", 0),
            "missing_checks": lambda x: x["required_status_checks"].pop("checks"),
            "not_strict": lambda x: x["required_status_checks"].__setitem__("strict", False),
            "contexts_added": lambda x: x["required_status_checks"]["contexts"].append("synthetic/extra"),
            "contexts_duplicate": lambda x: x["required_status_checks"]["contexts"].append("synthetic/llvm-security"),
            "contexts_wrong_type": lambda x: x["required_status_checks"].__setitem__("contexts", "synthetic/llvm-security"),
            "missing_contexts": lambda x: x["required_status_checks"].pop("contexts"),
            "malformed": lambda x: x["required_status_checks"].__setitem__("checks", "garbage"),
            "invalid_type": lambda x: x["required_status_checks"].__setitem__("checks", [{"context": "synthetic/a", "app_id": "12345"}]),
            "unknown_entry": lambda x: x["required_status_checks"]["checks"][0].__setitem__("extra", "bad"),
        }
        for label, mutate in scenarios.items():
            with self.subTest(label=label):
                p, o = fixture()
                mutate(o)
                with self.assertRaises(checker.Denied):
                    checker.verify(p, o)

    def test_reject_mutated_expected_policy(self):
        scenarios = {
            "pending": lambda x: x.__setitem__("state", "UNAPPROVED"),
            "unknown_repo": lambda x: x.__setitem__("repository", "other/private"),
            "wrong_branch": lambda x: x.__setitem__("branch", "feature"),
            "old_version": lambda x: x.__setitem__("version", 0),
            "bool_version": lambda x: x.__setitem__("version", True),
            "empty": lambda x: x.__setitem__("checks", []),
            "duplicate": lambda x: x["checks"].append(copy.deepcopy(x["checks"][0])),
            "duplicate_context": lambda x: x["checks"].append({"context":"synthetic/llvm-security", "app_id":67890}),
            "unknown_app": lambda x: x["checks"][0].__setitem__("app_id", 42),
            "zero_app": lambda x: x["checks"][0].__setitem__("app_id", 0),
            "boolean_app": lambda x: x["checks"][0].__setitem__("app_id", True),
            "whitespace_context": lambda x: x["checks"][0].__setitem__("context", " synthetic/llvm-security"),
            "extra_key": lambda x: x.__setitem__("untrusted", "yes"),
        }
        for label, mutate in scenarios.items():
            with self.subTest(label=label):
                p, o = fixture()
                mutate(p)
                with self.assertRaises(checker.Denied):
                    checker.verify(p, o)

    def test_cli_reads_protection_from_stdin(self):
        p, o = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            policy = Path(tmp) / "policy.json"
            policy.write_text(json.dumps(p))
            cmd = [sys.executable, "-I", "-B", str(MODULE), str(policy), "-"]
            good = subprocess.run(cmd, input=json.dumps(o), capture_output=True, text=True)
            self.assertEqual(good.returncode, 0, good.stderr)
            o["required_status_checks"]["checks"][0]["app_id"] = 1
            bad = subprocess.run(cmd, input=json.dumps(o), capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)

    def test_cli_is_fail_closed(self):
        p, o = fixture()
        with tempfile.TemporaryDirectory() as tmp:
            policy = Path(tmp)/"policy.json"
            state = Path(tmp)/"state.json"
            policy.write_text(json.dumps(p)); state.write_text(json.dumps(o))
            cmd = [sys.executable, "-I", "-B", str(MODULE), str(policy), str(state)]
            ok = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(ok.returncode, 0, ok.stderr)
            p["state"] = "UNAPPROVED"
            policy.write_text(json.dumps(p))
            denied = subprocess.run(cmd, capture_output=True, text=True)
            self.assertNotEqual(denied.returncode, 0)
            self.assertIn("DENIED", denied.stderr)


if __name__ == "__main__":
    unittest.main(verbosity=2)
