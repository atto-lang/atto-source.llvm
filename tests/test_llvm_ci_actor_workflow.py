#!/usr/bin/env python3
"""Fail-closed regression for the compiled .ores actor workflow command.

No PyYAML dependency: this checks the exact expected critical step declarations.
It is not a substitute for GitHub's own workflow parser or executed guest tests.
"""
import pathlib
import re
import unittest

WORKFLOW = (pathlib.Path(__file__).resolve().parents[1] /
            ".github/workflows/ci.yml")


class CompiledActorWorkflowTests(unittest.TestCase):
    def test_ctest_command_is_complete_and_not_injected_into_another_step(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        command = ("ctest --test-dir build -R "
                   "'^wasm32_source_actor_guest_lifecycle$' "
                   "--output-on-failure --timeout 120")
        self.assertIn("      - name: Execute actual compiled source actor WASM lifecycle guest\n"
                      "        shell: bash\n        run: |\n"
                      "          set -euo pipefail\n"
                      "          " + command + "\n", source)
        self.assertEqual(source.count(command), 1)
        self.assertNotIn("lifecycle      - name:", source)
        self.assertNotIn("\n --output-on-failure --timeout 120", source)

    def test_provenance_and_actor_stages_are_unique(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        names = re.findall(r"(?m)^      - name: ([^\n]+)$", source)
        self.assertEqual(len(names), len(set(names)), "duplicate CI step name")
        for required in [
            "Verify exact source revision",
            "Capture exact-source compiler receipt on trusted build runner",
            "Require exact compiled source actor CTest registration",
            "Execute actual compiled source actor WASM lifecycle guest",
            "Reverify compiler/source after compiled guest actor execution",
            "Verify parser, IR and native execution",
            "Reverify compiler/source after complete CTest suite",
        ]:
            self.assertEqual(names.count(required), 1, required)
        self.assertLess(names.index("Require exact compiled source actor CTest registration"),
                        names.index("Execute actual compiled source actor WASM lifecycle guest"))
        self.assertLess(names.index("Execute actual compiled source actor WASM lifecycle guest"),
                        names.index("Reverify compiler/source after compiled guest actor execution"))


    def test_prebuild_checkout_requires_clean_and_no_ignored_source(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        start = source.index("      - name: Verify exact source revision\\n")
        end = source.index("      - name: Reject malformed compiled actor workflow stages\\n", start)
        self.assertLess(end, source.index("      - name: Configure\\n"))
        segment = source[start:end]
        for guard in (
            'test "$(git rev-parse HEAD)" = "$EXPECTED_HEAD"',
            'test "$(git rev-parse --show-toplevel)" = "$GITHUB_WORKSPACE"',
            'git status --porcelain --untracked-files=no',
            'test ! -e build',
            'test ! -L build',
            'git ls-files --others --exclude-standard',
            'git ls-files --others --ignored --exclude-standard',
        ):
            self.assertIn(guard, segment)
        self.assertLess(segment.index("git status --porcelain"), segment.index("test ! -e build"))
        self.assertLess(segment.index("git ls-files --others --exclude-standard"),
                        segment.index("git ls-files --others --ignored --exclude-standard"))


if __name__ == "__main__":
    unittest.main()
