#!/usr/bin/env python3
"""Fail-closed, source-only security regression for LLVM conformance CI.

This verifies workflow configuration; it is not native compiler evidence and
does not authorize private source transfer or cross-organization checkout.
"""
import hashlib
import re
from pathlib import Path
import unittest

WORKFLOW = (Path(__file__).resolve().parents[1] /
            ".github/workflows/llvm-conformance.yml")
# The reviewed exact workflow revision. Deliberate workflow edits must update
# this pin in a security-reviewed change; branch protection must enforce review.
APPROVED_WORKFLOW_SHA256 = "4bef04bf64bbd26a97fcc855c4108c3b36e91466105611dd813d971fb5a14f88"
ACTIONS = (
    "actions/checkout@11bd71901bbe5b1630ceea73d27597364c9af683",
    "actions/setup-node@49933ea5288caeca8642d1e84afbd3f7d6820020",
    "actions/upload-artifact@ea165f8d65b6e75b540449e92b4886f43607fa02",
)
# Pin the security-sensitive workflow admission envelope. This simple
# source-only guard intentionally rejects new triggers, token scopes or jobs
# until they are separately reviewed; it is not an authorization boundary.
APPROVED_PREFIX = (
    "name: LLVM conformance contract\n"
    "on:\n"
    "  push:\n"
    "    branches: [main]\n"
    "  pull_request:\n"
    "  workflow_dispatch:\n"
    "\n"
    "permissions:\n"
    "  contents: read\n"
    "\n"
    "jobs:\n"
)
APPROVED_SELF_CHECK = (
    "      - name: Audit minimal workflow permissions, immutable Actions and credential-free checkout\n"
    "        run: python3 scripts/test_llvm_conformance_workflow_security.py -v\n"
)


def problems(text):
    errors = []
    if hashlib.sha256(text.encode("utf-8")).hexdigest() != APPROVED_WORKFLOW_SHA256:
        errors.append("workflow content differs from the reviewed immutable digest")
    if not text.startswith(APPROVED_PREFIX):
        errors.append("unreviewed workflow trigger or top-level permissions")
    if len(re.findall(r"(?m)^\s*permissions\s*:", text)) != 1:
        errors.append("job-scoped or duplicate permissions must not override read-only policy")
    if len(re.findall(r"(?m)^on\s*:", text)) != 1:
        errors.append("duplicate or unreviewed workflow event declaration")
    if len(re.findall(r"(?m)^jobs\s*:", text)) != 1:
        errors.append("unexpected jobs mapping")
    else:
        # Include one-line YAML mappings, not only `job:` on its own line.
        # Otherwise `extra: {runs-on: ...}` evades the job-name gate.
        names = re.findall(r"(?m)^  ([a-z][a-z0-9-]*):(?:[ \t]|$)",
                           text.split("\njobs:\n", 1)[-1])
        if names != ["conformance"]:
            errors.append("unreviewed additional or renamed CI job")
    runners = re.findall(r"(?m)^    runs-on:\s*(\S.*?)\s*$", text)
    if runners != ["ubuntu-24.04"]:
        errors.append("the conformance job must use the reviewed GitHub-hosted runner")
    if text.count(APPROVED_SELF_CHECK) != 1:
        errors.append("workflow must execute the security guard after checkout")
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

    def test_reviewed_workflow_digest_and_quoted_yaml_mutations(self):
        clean = WORKFLOW.read_text()
        self.assertEqual(
            hashlib.sha256(clean.encode("utf-8")).hexdigest(),
            APPROVED_WORKFLOW_SHA256)
        for mutated in (
            clean + '\n  "new-job": {runs-on: ubuntu-latest, steps: [{run: echo bad}]}\n',
            clean + '\n"on": {workflow_run: {types: [completed]}}\n',
            clean.replace(APPROVED_SELF_CHECK,
                          "      - name: Unreviewed execution before guard\n"
                          "        run: echo bad\n" + APPROVED_SELF_CHECK),
            clean.replace("run: python3 scripts/test_ai_conformance_smoke.py",
                          "run: echo suspicious"),
        ):
            with self.subTest(case=mutated[:75]):
                self.assertTrue(problems(mutated))

    def test_each_workflow_byte_mutation_invalidates_reviewed_digest(self):
        approved = bytearray(WORKFLOW.read_bytes())
        self.assertEqual(len(approved), 4502)
        for index in range(len(approved)):
            mutated = bytearray(approved)
            mutated[index] ^= 1
            self.assertNotEqual(hashlib.sha256(mutated).hexdigest(),
                                APPROVED_WORKFLOW_SHA256)

    def test_event_scope_and_security_guard_bypasses_are_rejected(self):
        clean = WORKFLOW.read_text()
        self.assertFalse(problems(clean))
        for mutated in (
            clean.replace("  contents: read\n", "  contents: read\n  actions: write\n"),
            clean.replace("    runs-on: ubuntu-24.04\n",
                          "    runs-on: ubuntu-24.04\n    permissions:\n      actions: write\n"),
            clean.replace("  workflow_dispatch:\n", "  workflow_dispatch:\n  workflow_run:\n"),
            clean + "\n  unrestricted:\n    runs-on: ubuntu-latest\n"
                    "    steps:\n      - run: echo unreviewed\n",
            clean.replace(APPROVED_SELF_CHECK, ""),
            clean + "\n  smuggled: {runs-on: ubuntu-24.04, steps: [{run: echo bad}]}\n",
            clean + "\non: {workflow_run: {types: [completed]}}\n",
            clean.replace("    runs-on: ubuntu-24.04", "    runs-on: self-hosted"),
            clean.replace("    timeout-minutes: 20\n",
                          "    runs-on: self-hosted\n    timeout-minutes: 20\n"),
        ):
            with self.subTest(fragment=mutated[:120]):
                self.assertTrue(problems(mutated))

    def test_all_120_single_character_action_pin_mutations_are_rejected(self):
        # An attacker must not change even one nibble of a reviewed Action.
        clean = WORKFLOW.read_text()
        mutations = 0
        for immutable in ACTIONS:
            prefix, sha = immutable.split("@")
            self.assertEqual(len(sha), 40)
            for pos, letter in enumerate(sha):
                substitute = "0" if letter != "0" else "1"
                changed = sha[:pos] + substitute + sha[pos + 1:]
                with self.subTest(action=prefix, position=pos):
                    self.assertTrue(problems(clean.replace(
                        immutable, prefix + "@" + changed)))
                mutations += 1
        self.assertEqual(mutations, 120)

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
