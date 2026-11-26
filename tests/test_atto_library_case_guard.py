import pathlib
import subprocess
import unittest

from atto_library_case_guard import REQUIRED, validate_corpus, validate_case, rejection_valid


class LibraryCaseGuardTests(unittest.TestCase):
    def test_complete_and_extra_inventory(self):
        cases = [pathlib.Path(n) for n in REQUIRED]
        validate_corpus(cases)
        validate_corpus(cases + [pathlib.Path('extra.atto')])
        with self.assertRaises(ValueError):
            validate_corpus([])
        for case in cases:
            with self.subTest(case=case), self.assertRaises(ValueError):
                validate_corpus([p for p in cases if p != case])

    def test_fixture_polarity_and_explicit_exit(self):
        validate_case('core_num.atto', {'exit': 42})
        validate_case('neg_unknown_core.atto', {'reject': True})
        for name, meta in [('core_num.atto', {'reject': True}),
                           ('neg_unknown_core.atto', {'exit': 0}),
                           ('extra.atto', {})]:
            with self.subTest(name=name), self.assertRaises(ValueError):
                validate_case(name, meta)

    def test_rejection_outcomes(self):
        def result(code, stdout='', stderr=''):
            return subprocess.CompletedProcess([], code, stdout, stderr)
        self.assertTrue(rejection_valid(result(1, stderr='unknown function'), ['unknown']))
        for r, errors in [(result(0, stderr='unknown function'), ['unknown']),
                          (result(-9, stderr='unknown function'), ['unknown']),
                          (result(1), []),
                          (result(1, stdout='IR', stderr='unknown function'), ['unknown']),
                          (result(1, stderr='different error'), ['unknown'])]:
            with self.subTest(result=r):
                self.assertFalse(rejection_valid(r, errors))


if __name__ == '__main__':
    unittest.main()
