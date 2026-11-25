"""Source-only adversarial tests for derived C++ numeric AttoLang source embedding."""
from __future__ import annotations
import importlib.util
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("numeric_gen", ROOT / "scripts/generate_atto_numeric_embed.py")
assert spec is not None and spec.loader is not None
numeric_gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(numeric_gen)


class NumericEmbedIntegrityTest(unittest.TestCase):
    def test_exact_source_renders_committed_generated_literal(self):
        self.assertEqual(0, numeric_gen.run(ROOT, check=True))

    def test_invalid_source_blob_rejected(self):
        source = (ROOT / numeric_gen.SOURCE).read_bytes()
        with self.assertRaisesRegex(ValueError, "identity mismatch"):
            numeric_gen.render(source + b"\n")

    def test_empty_source_rejected(self):
        with self.assertRaisesRegex(ValueError, "empty"):
            numeric_gen.render(b"")

    def test_oversized_source_rejected(self):
        with self.assertRaisesRegex(ValueError, "exceeds"):
            numeric_gen.render(b"a" * (1024 * 1024 + 1))

    def test_invalid_utf8_rejected(self):
        with self.assertRaises(UnicodeDecodeError):
            numeric_gen.render(b"\xff")

    def test_generated_embed_mutation_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            (temp / numeric_gen.SOURCE).parent.mkdir(parents=True)
            (temp / numeric_gen.OUTPUT).parent.mkdir(parents=True)
            (temp / numeric_gen.SOURCE).write_bytes((ROOT / numeric_gen.SOURCE).read_bytes())
            (temp / numeric_gen.OUTPUT).write_bytes((ROOT / numeric_gen.OUTPUT).read_bytes() + b"\n")
            with self.assertRaisesRegex(ValueError, "differs"):
                numeric_gen.run(temp, check=True)

    def test_source_symlink_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            temp = Path(td)
            (temp / numeric_gen.SOURCE).parent.mkdir(parents=True)
            (temp / numeric_gen.SOURCE).symlink_to(ROOT / numeric_gen.SOURCE)
            with self.assertRaisesRegex(ValueError, "non-symlink"):
                numeric_gen.run(temp, check=True)


if __name__ == "__main__":
    unittest.main()
