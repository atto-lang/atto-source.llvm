#!/usr/bin/env python3
"""Adversarial regression tests for native manifest/header parity checker."""
import copy
import json
import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from native_core_error_catalog_sync import validate_contract

ROOT = pathlib.Path(__file__).resolve().parents[1]
MANIFEST = json.loads((ROOT / "contracts/v1/error-catalog.json").read_text(encoding="utf-8"))
HEADER = (ROOT / "src/main/cpp/oreslang/runtime/NativeCoreError.hpp").read_text(encoding="utf-8")
TOKEN = "inline constexpr std::array<CoreErrorDescriptor, 99> core_error_catalog{{"


class CatalogVerifierTests(unittest.TestCase):
    def test_current_sources_are_exact(self):
        self.assertEqual(99, validate_contract(MANIFEST, HEADER))

    def must_fail(self, manifest=MANIFEST, header=HEADER):
        with self.assertRaises(ValueError):
            validate_contract(manifest, header)

    def test_missing_or_commented_out_catalog(self):
        self.must_fail(header=HEADER.replace(TOKEN, "// " + TOKEN, 1))
        start = HEADER.index(TOKEN)
        end = HEADER.index("}};", start) + 3
        self.must_fail(header=HEADER[:start] + "/* " + HEADER[start:end] + " */" + HEADER[end:])

    def test_duplicate_active_catalog(self):
        start = HEADER.index(TOKEN)
        end = HEADER.index("}};", start) + 3
        self.must_fail(header=HEADER + "\n" + HEADER[start:end])

    def test_header_code_and_interface_drift(self):
        self.must_fail(header=HEADER.replace('{"ActorError", 9000, 0, true, false, true},',
                                             '{"ActorError", 9001, 0, true, false, true},', 1))
        self.must_fail(header=HEADER.replace('{"ActorError", 9000, 0, true, false, true},',
                                             '{"ActorError", 9000, 0, true, false, false},', 1))

    def test_extra_non_row_content_is_rejected(self):
        self.must_fail(header=HEADER.replace(TOKEN, TOKEN + "\n garbage", 1))

    def test_duplicate_or_invalid_manifest(self):
        altered = copy.deepcopy(MANIFEST)
        altered["entries"][1]["code"] = 0
        self.must_fail(manifest=altered)
        altered = copy.deepcopy(MANIFEST)
        altered["entries"][1]["parent"] = "ActorError"  # forward parent/cycle
        self.must_fail(manifest=altered)
        altered = copy.deepcopy(MANIFEST)
        altered["entries"][0]["interfaces"].append("NotAnInterface")
        self.must_fail(manifest=altered)
        altered = copy.deepcopy(MANIFEST)
        altered["entries"][0]["abstract"] = True
        self.must_fail(manifest=altered)

    def test_native_bound_must_match(self):
        self.must_fail(header=HEADER.replace(TOKEN,
                        "inline constexpr std::array<CoreErrorDescriptor, 98> core_error_catalog{{", 1))


if __name__ == "__main__":
    unittest.main()
