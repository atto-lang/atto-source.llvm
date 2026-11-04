"""Source-free negative mutation tests: never count as compiler-backed guest work."""
import copy
import json
from pathlib import Path
import tempfile
import unittest
from mcp_m1_verify_native_receipt import load_receipt, validate

HEAD = "a" * 40
TREE = "b" * 40
NATIVE = "c" * 64
DESCRIPTOR = "d" * 64
METADATA = "e" * 64


def valid_receipt():
    return {
        "schema": "atto.mcp.llvm-aot-guest-evidence.v1",
        "evidenceClassification": "COMPILER_BACKED_NATIVE_GUEST_EXECUTED",
        "headSha": HEAD, "treeSha": TREE, "compilerSha256": "f" * 64,
        "cases": [{
            "sourceExtension": ext, "optimization": opt, "exitCode": 42,
            "nativeSha256": NATIVE, "descriptorSha256": DESCRIPTOR,
            "compilerMetadataSha256": METADATA, "descriptorBytes": 512
        } for ext in ("atto", "ores") for opt in ("-O0", "-O2")],
        "defaultRetention": "ABSENT_AT_O0_AND_O2",
        "negativeAdmissions": "PASS",
        "scope": "M1_ELF_GUEST_DESCRIPTOR_ONLY_NOT_MCP_SERVER",
    }


class NativeReceiptMutations(unittest.TestCase):
    def assert_rejects(self, transform):
        doc = valid_receipt()
        transform(doc)
        with self.assertRaises(ValueError):
            validate(doc, HEAD, TREE)

    def test_expected_shape(self):
        self.assertTrue(validate(valid_receipt(), HEAD, TREE))

    def test_case_order_is_not_significant(self):
        doc = valid_receipt()
        doc["cases"].reverse()
        self.assertTrue(validate(doc, HEAD, TREE))

    def test_wrong_source_head(self):
        self.assert_rejects(lambda x: x.update(headSha="0" * 40))

    def test_wrong_tree(self):
        self.assert_rejects(lambda x: x.update(treeSha="0" * 40))

    def test_unknown_extra_field(self):
        self.assert_rejects(lambda x: x.update(steps=10))

    def test_missing_schema(self):
        self.assert_rejects(lambda x: x.pop("schema"))

    def test_false_status(self):
        self.assert_rejects(lambda x: x.update(evidenceClassification="NOT_EXECUTED"))

    def test_server_scope_claim(self):
        self.assert_rejects(lambda x: x.update(scope="MCP_SERVER_EXECUTED"))

    def test_wrong_native_result(self):
        self.assert_rejects(lambda x: x["cases"][0].update(exitCode=0))

    def test_boolean_exit_code(self):
        self.assert_rejects(lambda x: x["cases"][0].update(exitCode=True))

    def test_duplicate_case(self):
        self.assert_rejects(lambda x: x["cases"].__setitem__(3, copy.deepcopy(x["cases"][0])))

    def test_missing_case(self):
        self.assert_rejects(lambda x: x["cases"].pop())

    def test_unknown_optimization(self):
        self.assert_rejects(lambda x: x["cases"][0].update(optimization="-O3"))

    def test_unknown_source_extension(self):
        self.assert_rejects(lambda x: x["cases"][0].update(sourceExtension="js"))

    def test_conflicting_descriptor_bytes(self):
        self.assert_rejects(lambda x: x["cases"][0].update(descriptorSha256="0" * 64))

    def test_conflicting_compiler_metadata(self):
        self.assert_rejects(lambda x: x["cases"][0].update(compilerMetadataSha256="0" * 64))

    def test_conflicting_descriptor_sizes(self):
        self.assert_rejects(lambda x: x["cases"][0].update(descriptorBytes=100))

    def test_over_budget(self):
        self.assert_rejects(lambda x: x["cases"][0].update(descriptorBytes=8194))

    def test_boolean_descriptor_size(self):
        self.assert_rejects(lambda x: x["cases"][0].update(descriptorBytes=False))

    def test_fake_hash(self):
        self.assert_rejects(lambda x: x["cases"][0].update(nativeSha256="x" * 64))

    def test_fake_compiler_digest(self):
        self.assert_rejects(lambda x: x.update(compilerSha256="X" * 64))

    def test_wrong_default_retention(self):
        self.assert_rejects(lambda x: x.update(defaultRetention="PRESENT"))

    def test_negative_admissions_skipped(self):
        self.assert_rejects(lambda x: x.update(negativeAdmissions="NOT_EXECUTED"))

    def test_missing_case_key(self):
        self.assert_rejects(lambda x: x["cases"][0].pop("descriptorBytes"))

    def test_bad_type(self):
        self.assert_rejects(lambda x: x["cases"][0].update(sourceExtension=1))

    def test_duplicate_json_keys(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "receipt.json"
            path.write_text('{"headSha":"' + HEAD + '","headSha":"' + HEAD + '"}')
            with self.assertRaises(ValueError):
                load_receipt(path)

    def test_oversized_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "receipt.json"
            path.write_bytes(b" " * 32769)
            with self.assertRaises(ValueError):
                load_receipt(path)

    def test_json_nonfinite(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "receipt.json"
            path.write_text('{"result": NaN}')
            with self.assertRaises(ValueError):
                load_receipt(path)

    def test_valid_file(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "receipt.json"
            path.write_text(json.dumps(valid_receipt()))
            self.assertTrue(validate(load_receipt(path), HEAD, TREE))


if __name__ == "__main__":
    unittest.main()
