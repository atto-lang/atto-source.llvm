"""Offline tests for the read-only formatter corpus audit (no oresfmt install)."""
from __future__ import annotations

from contextlib import redirect_stdout
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest

SCRIPT = Path(__file__).with_name("audit-oresfmt-corpus.py")
SPEC = importlib.util.spec_from_file_location("oresfmt_corpus_audit", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
audit_tool = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(audit_tool)


class CorpusAuditTests(unittest.TestCase):
    def test_discovers_only_regular_ores_files_without_following_links(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            (root / "ok.ores").write_text("val x = 1;\n")
            (root / "not.txt").write_text("ignored")
            (root / "build").mkdir()
            (root / "build" / "generated.ores").write_text("generated")
            (root / "nested").mkdir()
            (root / "nested" / "second.ores").write_text("x")
            (root / "link.ores").symlink_to(root / "ok.ores")
            self.assertEqual(
                [p.name for p in audit_tool.source_files([root])],
                ["second.ores", "ok.ores"],
            )

    def test_never_changes_source_even_when_formatter_would_rewrite_it(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / "a.ores"
            source.write_bytes(b"before\n")
            binary = root / "fake-oresfmt"
            binary.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "from pathlib import Path\n"
                "sys.stdout.buffer.write(Path(sys.argv[-1]).read_bytes().replace(b'before', b'after'))\n"
            )
            binary.chmod(0o755)
            report = audit_tool.audit([source], str(binary), 10)
            self.assertEqual(report["would_change"], 1)
            self.assertEqual(report["format_errors"], 0)
            self.assertEqual(source.read_bytes(), b"before\n")
            json_file = root / "audit.json"
            with redirect_stdout(io.StringIO()):
                code = audit_tool.main([
                    "--formatter", str(binary),
                    "--strict", "--json", str(json_file), str(root),
                ])
            self.assertEqual(code, 1)
            self.assertEqual(json.loads(json_file.read_text())["would_change"], 1)
            self.assertEqual(source.read_bytes(), b"before\n")

    def test_reports_rejected_fixture_without_modifying_it(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            source = root / "invalid.ores"
            source.write_bytes(b"invalid source\n")
            binary = root / "rejecting-oresfmt"
            binary.write_text(
                "#!/usr/bin/env python3\n"
                "import sys\n"
                "print('expected rejection', file=sys.stderr)\n"
                "sys.exit(2)\n"
            )
            binary.chmod(0o755)
            report = audit_tool.audit([source], str(binary), 10)
            self.assertEqual(report["format_errors"], 1)
            self.assertEqual(report["formatted_successfully"], 0)
            self.assertEqual(source.read_bytes(), b"invalid source\n")


if __name__ == "__main__":
    unittest.main()
