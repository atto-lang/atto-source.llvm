"""Verify CTest's output-JUnit path semantics without any Atto source checkout."""
from __future__ import annotations

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

WORKFLOW = Path(__file__).resolve().parents[1] / '.github/workflows/atto-lru-llvm-m0.yml'


class NativeLruCtestPathTests(unittest.TestCase):
    def test_exact_workflow_junit_paths(self):
        source = WORKFLOW.read_text(encoding='utf-8')
        self.assertIn('ctest --test-dir build/atto-lru', source)
        self.assertIn('--output-junit lru-ctest.xml', source)
        self.assertIn('tests/atto_lru_junit_gate.py build/atto-lru/lru-ctest.xml', source)
        self.assertNotIn('--output-junit build/atto-lru/lru-ctest.xml', source)

    def test_ctest_report_is_relative_to_test_directory(self):
        for executable in ('cmake', 'ctest'):
            self.assertIsNotNone(shutil.which(executable), executable + ' missing')
        with tempfile.TemporaryDirectory(prefix='atto-lru-ctest-path-') as tmp:
            root = Path(tmp)
            src, build = root / 'source', root / 'build' / 'atto-lru'
            src.mkdir()
            (src / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.16)\n'
                'project(atto_lru_ctest_path NONE)\n'
                'enable_testing()\n'
                'add_test(NAME atto_lru_path_proof COMMAND ${CMAKE_COMMAND} -E true)\n',
                encoding='utf-8')
            subprocess.run(['cmake', '-S', str(src), '-B', str(build)],
                           capture_output=True, text=True, check=True, timeout=20)
            subprocess.run(['ctest', '--test-dir', str(build),
                            '--output-junit', 'lru-ctest.xml'],
                           cwd=root, capture_output=True, text=True,
                           check=True, timeout=20)
            self.assertTrue((build / 'lru-ctest.xml').is_file())
            self.assertFalse((root / 'lru-ctest.xml').exists())
            self.assertFalse((build / 'build' / 'atto-lru' / 'lru-ctest.xml').exists())


if __name__ == '__main__':
    unittest.main()
