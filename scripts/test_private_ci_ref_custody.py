#!/usr/bin/env python3
"""Source-local, source-free adversarial regression for V3 ref cleanup.

Requires only the publisher Python module; never transfers source or contacts GitHub.
This tests CURRENT fail-closed custody, not a future authorized export protocol.
"""
import ast
import importlib.util
import os
from pathlib import Path
from unittest import TestCase, main
from unittest.mock import patch

PUBLISHER = Path(__file__).with_name("publish-private-ci-source-v3.py")
SPEC = importlib.util.spec_from_file_location("atro_ref_custody_v3", PUBLISHER)
v3 = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(v3)
REF = "ci/authorized-" + "c" * 40
TREE_REF = "ci/oreslang-" + "a" * 40
EXPECTED = "c" * 40
DEST_ID = 1323599136


class RefCustodySourceFreeTests(TestCase):
    def test_publish_guard_is_first_statement_by_ast(self):
        functions = {
            n.name: n for n in ast.parse(PUBLISHER.read_text(encoding="utf-8")).body
            if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))
        }
        publish = functions["publish"]
        first = publish.body[0]
        self.assertIsInstance(first, ast.Expr)
        self.assertIsInstance(first.value, ast.Call)
        self.assertIsInstance(first.value.func, ast.Name)
        self.assertEqual(first.value.func.id, "require_atomic_ref_rollback")
        self.assertEqual(len(first.value.args), 0)
        hold = functions["require_atomic_ref_rollback"]
        self.assertEqual(len(hold.body), 1)
        self.assertIsInstance(hold.body[0], ast.Raise)
        self.assertIsInstance(hold.body[0].exc, ast.Call)
        self.assertIsInstance(hold.body[0].exc.func, ast.Name)
        self.assertEqual(hold.body[0].exc.func.id, "Denied")

    def test_export_denied_before_source_or_destination_access(self):
        # The hold must precede ALL input validation, source reads, token
        # lookup, plan checks, destination commands and ref writes.
        for dest in ("streempilot-test/browser-studio-e2e", "not a repo", ""):
            with self.subTest(dest=dest), (
                patch.object(v3, "gh_api") as api,
                patch.object(v3, "run_bounded_gh") as command,
                patch.object(v3, "protected_authority") as authority,
                patch.object(v3, "exact_source_tree") as source,
                patch.object(v3, "require_handoff_refs_absent") as refs,
            ):
                with self.assertRaisesRegex(v3.Denied, "private CI export disabled"):
                    v3.publish(dest, 834, "e" * 40, "ci/test-plans/llvm-pr834.json")
                api.assert_not_called()
                command.assert_not_called()
                authority.assert_not_called()
                source.assert_not_called()
                refs.assert_not_called()

    def test_flags_cannot_enable_private_exports(self):
        flags = {
            "ALLOW_PRIVATE_CI_EXPORT": "1",
            "ALLOW_UNSAFE_PRIVATE_CI_EXPORT": "true",
            "PRIVATE_CI_FORCE": "true",
            "CI_SKIP_ROLLBACK": "1",
        }
        with patch.dict(os.environ, flags):
            with self.assertRaisesRegex(v3.Denied, "private CI export disabled"):
                v3.require_atomic_ref_rollback()

    def test_all_rest_mutations_denied_before_network(self):
        # REST ref deletion lacks an expected-SHA precondition; checking the
        # currently observed ref SHA cannot make a name-based DELETE atomic.
        for method in ("DELETE", "POST", "PATCH", "PUT"):
            with self.subTest(method=method), (
                patch.object(v3, "trusted_github_cli") as executable,
                patch.object(v3, "github_api_environment") as environment,
                patch.object(v3, "run_bounded_gh") as command,
            ):
                with self.assertRaisesRegex(v3.Denied, "deletion is forbidden"):
                    v3._gh_ref_request("trusted/ci", REF, method)
                executable.assert_not_called()
                environment.assert_not_called()
                command.assert_not_called()

    def test_invalid_ref_name_denied_before_network(self):
        for candidate in ("ci/mutable", "refs/heads/main", "ci/../" + EXPECTED,
                          "ci/authorized-" + "a" * 39, "ci/authorized-" + "G" * 40):
            with self.subTest(candidate=candidate), (
                patch.object(v3, "run_bounded_gh") as command,
            ):
                with self.assertRaisesRegex(v3.Denied, "target is invalid"):
                    v3._gh_ref_request("trusted/ci", candidate, "GET")
                command.assert_not_called()

    def test_rollback_never_deletes_across_adversarial_histories(self):
        # Hypothetical schedules for a concurrent ref owner:
        # A; A->B; A->B->A (ABA); A->404; A->404->A; drift of
        # destination repository ID; ambiguous transport.
        schedules = {
            "matches": [EXPECTED],
            "moved": [EXPECTED, "b" * 40],
            "aba": [EXPECTED, "b" * 40, EXPECTED],
            "gone": [EXPECTED, None],
            "recreated": [EXPECTED, None, EXPECTED],
            "identity-change": [EXPECTED, "replacement-repo"],
            "ambiguous-transport": [EXPECTED, "unknown"],
            "never-existed": [None],
        }
        for kind, state in schedules.items():
            for ref in (TREE_REF, REF):
                with self.subTest(kind=kind, ref=ref), (
                    patch.object(v3, "live_ref_sha", side_effect=state) as lookup,
                    patch.object(v3, "_gh_ref_request") as request,
                    patch.object(v3, "run_bounded_gh") as command,
                    patch.object(v3, "gh_api") as api,
                ):
                    with self.assertRaisesRegex(
                        v3.Denied, "atomic compare-and-delete required"
                    ):
                        v3.delete_exact_ref("trusted/ci", ref, EXPECTED, DEST_ID)
                    lookup.assert_not_called()
                    request.assert_not_called()
                    command.assert_not_called()
                    api.assert_not_called()

    def test_repeated_cleanup_remains_denied(self):
        with patch.object(v3, "gh_api") as api:
            for attempt in range(3):
                with self.subTest(attempt=attempt):
                    with self.assertRaises(v3.Denied):
                        v3.delete_exact_ref("trusted/ci", REF, EXPECTED, DEST_ID)
            api.assert_not_called()

    def test_get_then_delete_model_is_not_safe_even_after_404(self):
        # This intentionally vulnerable *model* documents the ABA fault,
        # it is NOT an action against GitHub or a permitted cleanup routine.
        refs = {REF: EXPECTED}
        observed = refs[REF]
        refs[REF] = "b" * 40  # competing actor moved the ref
        del refs[REF]         # unsafe name-based DELETE would remove B
        self.assertEqual(observed, EXPECTED)
        self.assertNotIn(REF, refs)
        # Post-delete 404 does not prove the deleted ref was still A.
        self.assertNotEqual("b" * 40, observed)


if __name__ == "__main__":
    main(verbosity=2)
