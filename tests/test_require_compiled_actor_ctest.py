import copy
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from require_compiled_actor_ctest import RegistrationFailure, verify_registration, NAME

SRC = Path('/source')
BUILD = SRC / 'build'


def good():
    return dict(kind='ctestInfo', version=dict(major=1, minor=0), tests=[
        dict(name=NAME, command=[
            '/usr/bin/python3', '/source/tests/wasm32_source_actor_guest.py',
            '--compiler', '/source/build/oreslang-llvmc',
            '--clang', '/usr/bin/clang', '--node', '/usr/bin/node',
            '--fixtures', '/source/tests/fixtures'],
            properties=[dict(name='TIMEOUT', value=120.0)])])


class RegistrationTests(unittest.TestCase):
    def verify(self, doc):
        verify_registration(doc, source_dir=SRC, build_dir=BUILD)

    def reject(self, doc):
        with self.assertRaises(RegistrationFailure):
            self.verify(doc)

    def test_valid_compiled_actor_registration(self):
        self.verify(good())

    def test_no_real_test(self):
        doc = good(); doc['tests'] = []; self.reject(doc)

    def test_duplicate_registration(self):
        doc = good(); doc['tests'].append(copy.deepcopy(doc['tests'][0])); self.reject(doc)

    def test_wrong_compiler(self):
        doc = good(); doc['tests'][0]['command'][3] = '/source/build/mock'; self.reject(doc)

    def test_wrong_harness(self):
        doc = good(); doc['tests'][0]['command'][1] = '/source/tests/skip.py'; self.reject(doc)

    def test_injected_extra_args(self):
        doc = good(); doc['tests'][0]['command'] += ['--skip']; self.reject(doc)

    def test_wrong_fixture_dir(self):
        doc = good(); doc['tests'][0]['command'][9] = '/source/tests/stubs'; self.reject(doc)

    def test_disabled_or_skip_return(self):
        for prop in ('DISABLED', 'SKIP_RETURN_CODE', 'WILL_FAIL', 'SKIP_REGULAR_EXPRESSION'):
            with self.subTest(prop=prop):
                doc = good(); doc['tests'][0]['properties'].append(dict(name=prop, value=True)); self.reject(doc)

    def test_wrong_timeout(self):
        doc = good(); doc['tests'][0]['properties'][0]['value'] = 15; self.reject(doc)

    def test_wrong_interpreter(self):
        doc = good(); doc['tests'][0]['command'][0] = '/usr/bin/true'; self.reject(doc)

    def test_wrong_json_schema(self):
        doc = good(); doc['kind'] = 'fake'; self.reject(doc)

    def test_nonabsolute_executable(self):
        doc = good(); doc['tests'][0]['command'][7] = 'node'; self.reject(doc)


if __name__ == '__main__':
    unittest.main()
