#!/usr/bin/env python3
"""Negative mutation tests for the 50-program compiler fixture inventory.

These are HOST-ONLY manifest checks, never guest or compiler proof.
"""
import copy
import json
import pathlib
import tempfile
import unittest

from ownership_compiler_fixture_matrix import MatrixFailure, load_cases, MANIFEST


class FixtureInventoryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pristine = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def reject_mutation(self, mutate):
        artifact = copy.deepcopy(self.pristine)
        mutate(artifact)
        with tempfile.TemporaryDirectory(prefix="atto-fixture-mutation-") as tmp:
            fixture = pathlib.Path(tmp) / "mutation.json"
            fixture.write_text(json.dumps(artifact), encoding="utf-8")
            with self.assertRaises(MatrixFailure):
                load_cases(fixture)

    def test_exact_full_matrix_and_unique_programs(self):
        fixtures = load_cases()
        self.assertEqual(len(fixtures), 50)
        self.assertEqual(len(set(x["id"] for x in fixtures)), 50)
        self.assertEqual(len(set(x["source"] for x in fixtures)), 50)

    def test_missing_fixture_denied(self):
        self.reject_mutation(lambda x: x["cases"].pop())

    def test_additional_fixture_denied(self):
        self.reject_mutation(lambda x: x["cases"].append(copy.deepcopy(x["cases"][-1])))

    def test_duplicate_fixture_id_denied(self):
        self.reject_mutation(lambda x: x["cases"][1].update(id=x["cases"][0]["id"]))

    def test_duplicate_source_denied(self):
        self.reject_mutation(lambda x: x["cases"][1].update(source=x["cases"][0]["source"]))

    def test_wrong_semantic_diagnostic_denied(self):
        self.reject_mutation(lambda x: x["cases"][0].update(expected_diagnostic="parse error"))

    def test_negative_without_diagnostic_denied(self):
        def mutate(x):
            del x["cases"][0]["expected_diagnostic"]
            x["cases"][0]["expected_guest_exit"] = 42
        self.reject_mutation(mutate)

    def test_positive_without_guest_oracle_denied(self):
        self.reject_mutation(lambda x: x["cases"][49].pop("expected_guest_exit"))

    def test_positive_wrong_exit_type_denied(self):
        self.reject_mutation(lambda x: x["cases"][49].update(expected_guest_exit=True))

    def test_category_swap_denied(self):
        self.reject_mutation(lambda x: x["cases"][0].update(category="borrowed-field-share"))

    def test_disabled_real_compiler_gate_denied(self):
        self.reject_mutation(
            lambda x: x["requirements"].update(real_compiler_binary=False))

    def test_disabled_clang_guest_gate_denied(self):
        self.reject_mutation(
            lambda x: x["requirements"].update(clang_link_and_guest_exit_on_positive=False))

    def test_forged_executed_status_denied(self):
        self.reject_mutation(lambda x: x.update(classification="EXECUTED_AND_VERIFIED"))

    def test_schema_mismatch_denied(self):
        self.reject_mutation(lambda x: x.update(schema_version=2))

    def test_extra_unknown_authority_denied(self):
        self.reject_mutation(lambda x: x["cases"][0].update(verified=True))

    def test_path_traversal_fixture_name_denied(self):
        self.reject_mutation(lambda x: x["cases"][0].update(id="../escape"))

    def test_category_underfilled_denied(self):
        self.reject_mutation(lambda x: x["cases"][0].update(category="owned-scalar-copy"))


if __name__ == "__main__":
    unittest.main()
