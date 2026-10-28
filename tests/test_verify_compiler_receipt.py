#!/usr/bin/env python3
"""Source-free adversarial tests: actual temp Git tree, never a fabricated SHA pair."""
import json
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_compiler_receipt import GateFailure, run


class ReceiptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "checkout"
        self.root.mkdir()
        for args in (("init", "-q"), ("config", "user.email", "ci@example.invalid"),
                     ("config", "user.name", "CI")):
            subprocess.run(("git", "-C", str(self.root), *args), check=True, capture_output=True)
        (self.root / "source.ores").write_text("define actor A as end\n")
        subprocess.run(("git", "-C", str(self.root), "add", "source.ores"), check=True)
        subprocess.run(("git", "-C", str(self.root), "commit", "-qm", "fixture"), check=True)
        self.head = subprocess.check_output(("git", "-C", str(self.root), "rev-parse", "HEAD"), text=True).strip()
        build = self.root / "build"
        build.mkdir()
        self.binary = build / "oreslang-llvmc"
        self.binary.write_bytes(b"fake binary for receipt gate only\n")
        self.binary.chmod(0o755)
        self.receipt = Path(self.temp.name) / "receipt.json"

    def call(self, mode="verify", **args):
        kw = dict(source=str(self.root), compiler=str(self.binary),
                  expected_head=self.head, receipt=str(self.receipt))
        kw.update(args)
        return run(mode, **kw)

    def test_capture_and_verify_local_identity_only(self):
        self.call("capture")
        self.call("verify")
        self.assertEqual(json.loads(self.receipt.read_text())["sourceCommit"], self.head)

    def test_wrong_expected_head_rejected(self):
        with self.assertRaisesRegex(GateFailure, "head or tree"):
            self.call("capture", expected_head="f" * 40)

    def test_dirty_tracked_checkout_rejected(self):
        (self.root / "source.ores").write_text("changed")
        with self.assertRaisesRegex(GateFailure, "dirty"):
            self.call("capture")

    def test_compiler_tampering_after_capture_rejected(self):
        self.call("capture")
        self.binary.write_bytes(b"tampered executable\n")
        with self.assertRaisesRegex(GateFailure, "receipt does not match"):
            self.call("verify")

    def test_receipt_tampering_rejected(self):
        self.call("capture")
        doc = json.loads(self.receipt.read_text())
        doc["sourceTree"] = "a" * 40
        self.receipt.write_text(json.dumps(doc))
        with self.assertRaisesRegex(GateFailure, "receipt does not match"):
            self.call("verify")

    def test_reused_receipt_capture_refused(self):
        self.call("capture")
        with self.assertRaises(FileExistsError):
            self.call("capture")

    def test_missing_receipt_rejected(self):
        with self.assertRaises(FileNotFoundError):
            self.call("verify")

    def test_binary_symlink_rejected(self):
        target = self.root / "build" / "copy"
        shutil.copy2(self.binary, target)
        self.binary.unlink()
        self.binary.symlink_to(target)
        with self.assertRaisesRegex(GateFailure, "non-symlink"):
            self.call("capture")

    def test_binary_outside_build_rejected(self):
        outside = Path(self.temp.name) / "compiler"
        shutil.copy2(self.binary, outside)
        with self.assertRaisesRegex(GateFailure, "in-tree"):
            self.call("capture", compiler=str(outside))

    def test_nonexecutable_compiler_rejected(self):
        self.binary.chmod(0o644)
        with self.assertRaisesRegex(GateFailure, "regular executable"):
            self.call("capture")

    def test_source_root_subdirectory_rejected(self):
        with self.assertRaisesRegex(GateFailure, "exact Git checkout root"):
            self.call("capture", source=str(self.root / "build"))

    def test_wrong_schema_and_extra_key_rejected(self):
        self.call("capture")
        doc = json.loads(self.receipt.read_text())
        doc["guestRuntimePass"] = True
        self.receipt.write_text(json.dumps(doc))
        with self.assertRaisesRegex(GateFailure, "unexpected or missing fields"):
            self.call("verify")

    def test_receipt_symlink_rejected(self):
        self.call("capture")
        dest = Path(self.temp.name) / "alias.json"
        self.receipt.rename(dest)
        self.receipt.symlink_to(dest)
        with self.assertRaisesRegex(GateFailure, "unsafe compiler receipt"):
            self.call("verify")

if __name__ == "__main__":
    unittest.main()
