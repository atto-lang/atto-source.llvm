#!/usr/bin/env python3
"""Fail-closed, source-only security regression for LLVM conformance CI.

This verifies workflow configuration; it is not native compiler evidence and
does not authorize private source transfer or cross-organization checkout.
"""
import re
from pathlib import Path
import unittest

WORKFLOW = (Path(__file__).resolve().parents[1] /
            ".github/workflows/llvm-conformance.yml")
ACTIONS = (
    "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
    "actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020",
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
)


def problems(text):
    errors = []
    found = re.findall(r"(?m)^\s*- uses:\s*(\S+)", text)
    if tuple(found) != ACTIONS:
        errors.append("workflow actions must be exactly the reviewed immutable SHAs")
    if "permissions:\n  contents: read\n" not in text:
        errors.append("workflow must use minimum read-only repository permissions")
    if re.search(r"(?m)^\s*(?:pull_request_target|id-token:\s*write|contents:\s*write)\b", text):
        errors.append("privileged workflow event or permission is forbidden")
    if re.search(r"\$\{\{\s*secrets\s*\.", text, flags=re.IGNORECASE):
        errors.append("conformance tests must not receive GitHub secrets")
    checkout = re.search(
        r"(?m)^      - uses: actions/checkout@[0-9a-f]{40}[^\n]*\n"
        r"        with:\n          persist-credentials: false(?:\n|$)", text
    )
    if checkout is None:
        errors.append("checkout must explicitly disable persisted credentials")
    if re.search(r"(?m)^\s*persist-credentials:\s*(?!false\s*$)\S+", text):
        errors.append("persist-credentials must never be enabled")
    return errors


class WorkflowSecurityProof(unittest.TestCase):
    def test_real_workflow_is_source_only_and_fail_closed(self):
        self.assertEqual(problems(WORKFLOW.read_text()), [])

    def test_mutable_action_tag_is_rejected(self):
        clean = WORKFLOW.read_text()
        for immutable in ACTIONS:
            with self.subTest(action=immutable):
                self.assertTrue(problems(clean.replace(immutable,
                    immutable.split("@")[0] + "@v4")))

    def test_missing_and_enabled_checkout_credentials_are_rejected(self):
        clean = WORKFLOW.read_text()
        self.assertTrue(problems(clean.replace(
            "persist-credentials: false", "persist-credentials: true")))
        self.assertTrue(problems(clean.replace(
            "          persist-credentials: false\n", "")))

    def test_unreviewed_extra_action_is_rejected(self):
        clean = WORKFLOW.read_text()
        self.assertTrue(problems(clean.replace(
            "      - name: Verify AI smoke admission unit tests",
            "      - uses: external/unknown@v1\n"
            "      - name: Verify AI smoke admission unit tests")))

    def test_privilege_or_secret_injection_is_rejected(self):
        clean = WORKFLOW.read_text()
        for mutated in (
            clean.replace("  contents: read", "  contents: write"),
            clean.replace("  pull_request:", "  pull_request_target:"),
            clean.replace("  contents: read", "  contents: read\n  id-token: write"),
            clean.replace("      - name: Verify AI smoke admission unit tests",
                          "      - run: echo ${{ secrets.DANGEROUS }}\n"
                          "      - name: Verify AI smoke admission unit tests"),
            clean.replace("      - name: Verify AI smoke admission unit tests",
                          "      - run: echo ${{secrets.DANGEROUS}}\n"
                          "      - name: Verify AI smoke admission unit tests"),
            clean.replace("      - name: Verify AI smoke admission unit tests",
                          "      - run: echo ${{  secrets.DANGEROUS }}\n"
                          "      - name: Verify AI smoke admission unit tests"),
        ):
            with self.subTest(fragment=mutated[:60]):
                self.assertTrue(problems(mutated))


if __name__ == "__main__":
    unittest.main()
