import copy
import hashlib
import tempfile
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from require_compiled_actor_ctest import (RegistrationFailure, verify_registration,
                                          verify_guest_inputs, NAME, PINNED_GUEST_BLOBS)

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

 
class GuestInputPinsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        (self.root / 'tests').mkdir()
        self.target = self.root / 'tests/wasm32_source_actor_guest.py'
        self.data = b'compiled guest harness fixture for source-free tests\n'
        self.target.write_bytes(self.data)
        self.pin = hashlib.sha1(b'blob ' + str(len(self.data)).encode() +
                                b'\x00' + self.data).hexdigest()
        self.pins = {'tests/wasm32_source_actor_guest.py': self.pin}

    def test_valid_synthetic_git_blob(self):
        verify_guest_inputs(self.root, self.pins)

    def test_mutated_guest_harness_blob_rejected(self):
        self.target.write_bytes(b'pass\n')
        with self.assertRaisesRegex(RegistrationFailure, 'blob mismatch'):
            verify_guest_inputs(self.root, self.pins)

    def test_missing_guest_harness_rejected(self):
        self.target.unlink()
        with self.assertRaisesRegex(RegistrationFailure, 'unavailable'):
            verify_guest_inputs(self.root, self.pins)

    def test_symlinked_guest_harness_rejected(self):
        other = self.root / 'replacement'
        other.write_bytes(self.data)
        self.target.unlink()
        self.target.symlink_to(other)
        with self.assertRaisesRegex(RegistrationFailure, 'symlink'):
            verify_guest_inputs(self.root, self.pins)

    def test_parent_directory_symlink_rejected(self):
        (self.root / 'tests').rename(self.root / 'actual_tests')
        (self.root / 'tests').symlink_to(self.root / 'actual_tests')
        with self.assertRaisesRegex(RegistrationFailure, 'parent directory'):
            verify_guest_inputs(self.root, self.pins)

    def test_oversized_guest_harness_rejected(self):
        self.target.write_bytes(b'x' * 65537)
        with self.assertRaisesRegex(RegistrationFailure, 'bounded regular file'):
            verify_guest_inputs(self.root, self.pins)

    def test_invalid_pin_and_parent_traversal_rejected(self):
        for bad in ({'tests/wasm32_source_actor_guest.py': 'not-sha1'},
                    {'../outside.py': self.pin}):
            with self.subTest(bad=bad), self.assertRaises(RegistrationFailure):
                verify_guest_inputs(self.root, bad)

    def test_current_repository_harness_and_negative_fixtures_pinned(self):
        # This test deliberately needs the real source checkout; it is not a
        # synthetic success receipt or a substitute for compiled guest execution.
        self.assertEqual(len(PINNED_GUEST_BLOBS), 4)
        verify_guest_inputs(Path(__file__).resolve().parents[1])



if __name__ == '__main__':
    unittest.main()
