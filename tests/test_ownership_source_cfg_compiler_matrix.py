#!/usr/bin/env python3
"""Host-only negative mutation oracle; NEVER an actual compiled guest pass."""
import copy
import json
import pathlib
import tempfile
import unittest

from ownership_source_cfg_compiler_matrix import MatrixFailure, MANIFEST, load_cases


class SourceCfgManifestMutations(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.original = json.loads(MANIFEST.read_text(encoding="utf-8"))

    def denied(self, edit):
        data = copy.deepcopy(self.original)
        edit(data)
        with tempfile.TemporaryDirectory(prefix="source-cfg-mutant-") as tmp:
            path = pathlib.Path(tmp) / "manifest.json"
            path.write_text(json.dumps(data), encoding="utf-8")
            with self.assertRaises(MatrixFailure):
                load_cases(path)

    def test_01_all_fifty_unique_categories(self):
        data = load_cases()
        self.assertEqual(len(data), 50)
        self.assertEqual(len({x["source"] for x in data}), 50)

    def test_02_missing_case(self):
        self.denied(lambda d: d["cases"].pop())

    def test_03_duplicate_extra_case(self):
        self.denied(lambda d: d["cases"].append(copy.deepcopy(d["cases"][0])))

    def test_04_duplicate_id(self):
        self.denied(lambda d: d["cases"][1].update(id=d["cases"][0]["id"]))

    def test_05_duplicate_source(self):
        self.denied(lambda d: d["cases"][1].update(source=d["cases"][0]["source"]))

    def test_06_wrong_negative_semantic_diagnostic(self):
        self.denied(lambda d: d["cases"][3].update(
            expected_diagnostic="parser error"))

    def test_07_negative_converted_to_positive(self):
        def edit(d):
            d["cases"][3].pop("expected_diagnostic")
            d["cases"][3]["expected_guest_exit"] = 42
        self.denied(edit)

    def test_08_positive_converted_to_negative(self):
        def edit(d):
            d["cases"][0].pop("expected_guest_exit")
            d["cases"][0]["expected_diagnostic"] = "ownership violation"
        self.denied(edit)

    def test_09_fake_executed_status(self):
        self.denied(lambda d: d.update(classification="PASS_EXECUTED"))

    def test_10_disabled_clang_link(self):
        self.denied(lambda d: d["requirements"].update(
            clang_link_and_guest_exit_on_positive=False))

    def test_11_disabled_real_compiler(self):
        self.denied(lambda d: d["requirements"].update(
            real_compiler_binary=False))

    def test_12_disabled_path_sensitive_gate(self):
        self.denied(lambda d: d["requirements"].update(
            path_sensitive_source_restoration_contract=False))

    def test_13_missing_field_move(self):
        self.denied(lambda d: d["cases"][0].update(
            source=d["cases"][0]["source"].replace("rt take p.left",
                                                   "p.left")))

    def test_14_one_branch_forged_into_both_restorations(self):
        self.denied(lambda d: d["cases"][3].update(
            source=d["cases"][3]["source"].replace(
                "else { } fi", "else { p.left = saved; } fi")))

    def test_15_two_branch_case_loses_else_write(self):
        self.denied(lambda d: d["cases"][1].update(
            source=d["cases"][1]["source"].replace(
                "else { p.left = saved + 1; }",
                "else { }")))

    def test_16_missing_return_oracle(self):
        self.denied(lambda d: d["cases"][0].pop("expected_guest_exit"))

    def test_17_path_traversal_id(self):
        self.denied(lambda d: d["cases"][0].update(id="../escaped"))

    def test_18_unexpected_execution_authority(self):
        self.denied(lambda d: d["cases"][0].update(
            compiler_verified=True))

    def test_19_wrong_positive_direct_guest_exit(self):
        self.denied(lambda d: d["cases"][0].update(expected_guest_exit=99))

    def test_20_wrong_false_branch_guest_exit(self):
        # Sixth both-branch fixture must execute the else path.
        self.denied(lambda d: d["cases"][26].update(expected_guest_exit=25))

    def test_21_positive_true_branch_predicate_swap(self):
        def edit(d):
            row = d["cases"][1]
            row["source"] = row["source"].replace("if (20 eq 20)",
                                                  "if (20 eq 21)")
        self.denied(edit)

    def test_22_positive_false_branch_predicate_swap(self):
        def edit(d):
            row = d["cases"][26]
            row["source"] = row["source"].replace("if (25 eq 26)",
                                                  "if (25 eq 25)")
        self.denied(edit)

    def test_23_negative_false_branch_predicate_swap(self):
        def edit(d):
            row = d["cases"][28]
            row["source"] = row["source"].replace("if (25 eq 26)",
                                                  "if (25 eq 25)")
        self.denied(edit)

    def test_24_wrong_class_default_oracle(self):
        def edit(d):
            row = d["cases"][0]
            row["source"] = row["source"].replace(
                "pub let int left = 20;", "pub let int left = 30;")
        self.denied(edit)

    def test_25_wrong_class_identity(self):
        def edit(d):
            row = d["cases"][0]
            row["source"] = row["source"].replace(
                "define class Restore0 as", "define class Restore99 as")
        self.denied(edit)

    def test_26_wrong_class_constructor_identity(self):
        def edit(d):
            row = d["cases"][0]
            row["source"] = row["source"].replace(
                "new Restore0()", "new Restore1()")
        self.denied(edit)

    def test_27_duplicate_branch_predicate_not_accepted(self):
        def edit(d):
            row = d["cases"][1]
            row["source"] = row["source"].replace(
                "if (20 eq 20)", "if (20 eq 20) if (20 eq 20)")
        self.denied(edit)

    def test_28_out_of_range_positive_exit(self):
        self.denied(lambda d: d["cases"][1].update(expected_guest_exit=256))

    def test_29_false_branch_store_does_not_match_guest_exit(self):
        def edit(d):
            row = d["cases"][26]
            row["source"] = row["source"].replace(
                "else { p.left = saved + 1; }",
                "else { p.left = saved + 2; }")
        self.denied(edit)


if __name__ == "__main__":
    unittest.main()
