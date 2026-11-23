"""Source-only LLVM-local Atto arena gate. Not a native compiler or guest execution."""
import copy
import hashlib
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
CONTRACT = ROOT / 'contracts' / 'atto-llvm-arena-cleanup-v1.json'
SIGNED_MAX = 2**63 - 1


def alignment_valid(align):
    return align > 0 and (align & (align - 1)) == 0


def arena_end_offset(cursor, count, align, limit):
    if cursor < 0 or count < 0 or limit < 0 or cursor > limit or not alignment_valid(align):
        return -1
    if count == 0:
        return cursor
    pad = (-cursor) % align
    if pad > limit - cursor:
        return -1
    aligned = cursor + pad
    if count > limit - aligned:
        return -1
    return aligned + count


def arena_quiescent(*counts):
    return len(counts) == 5 and all(isinstance(n, int) and not isinstance(n, bool) and n == 0 for n in counts)


def arena_can_free(sealed, drops, *counts):
    return sealed is True and drops is True and arena_quiescent(*counts)


def arena_next_generation(generation, maximum):
    if generation < 1 or maximum < 2 or generation >= maximum or generation >= SIGNED_MAX:
        return -1
    return generation + 1


def arena_near_quota(used, quota, headroom):
    return used < 0 or quota < 0 or headroom < 0 or used > quota or headroom > quota or used >= quota - headroom


def arena_can_fit_request(used, request, quota):
    return used >= 0 and request >= 0 and quota >= 0 and used <= quota and request <= quota - used

FUNCS = {name: globals()[name] for name in ('arena_end_offset','arena_quiescent','arena_can_free','arena_next_generation','arena_near_quota','arena_can_fit_request')}


def validate(payload, root=ROOT):
    assert set(payload) == {'schema','status','source_paths','scope','reclamation','cases','negative_gates','evidence'}
    assert payload['schema'] == 'atto.llvm.local-arena-deterministic-cleanup.v1'
    assert payload['status'] == 'TARGET_SOURCE_ONLY_NOT_GUEST_EXECUTED'
    assert payload['scope']['backend'] == 'llvm' and payload['scope']['backend_local'] is True
    assert payload['scope']['byte_identical_across_compilers_required'] is False
    assert payload['scope']['public_profiles'] == ['core','std']
    assert payload['evidence']['guest_compiled'] is False
    assert payload['evidence']['guest_executed'] is False
    assert payload['evidence']['native_runtime_integrated'] is False
    assert payload['evidence']['no_gc_certified'] is False
    assert payload['evidence']['parity_certified'] is False
    assert len(payload['evidence']['backend_results']) == 11
    assert set(payload['evidence']['backend_results'].values()) == {'NOT_EXECUTED'}
    names = [x['path'] for x in payload['source_paths']]
    assert names == ['stdlib/policy/alloc/arena_lifecycle.atto','stdlib/policy/alloc/scoped_arena.atto',
                     'stdlib/policy/core/arena_limits.atto','stdlib/policy/std/arena_advice.atto']
    for x in payload['source_paths']:
        assert set(x) == {'path','sha256','bytes'}
        assert not Path(x['path']).is_absolute() and '..' not in Path(x['path']).parts
        src = root / x['path']
        assert not src.is_symlink() and src.is_file()
        b = src.read_bytes()
        assert not b.startswith(b'\xef\xbb\xbf') and b.endswith(b'\n') and b'\x00' not in b
        assert len(b) == x['bytes'] and hashlib.sha256(b).hexdigest() == x['sha256']
        txt = b.decode('utf8')
        assert not re.search(r'\b(?:syscall|malloc|free|thread|spawn|await|ffi|gc_collect)\s*\(',txt)
    core = (root / 'stdlib/policy/core/arena_limits.atto').read_text()
    alloc = (root / 'stdlib/policy/alloc/arena_lifecycle.atto').read_text()
    scoped = (root / 'stdlib/policy/alloc/scoped_arena.atto').read_text()
    std = (root / 'stdlib/policy/std/arena_advice.atto').read_text()
    for name in ('arena_alignment_valid','arena_padding','arena_end_offset'):
        assert re.search(r'pub fnc '+name+r'\(', core)
    for name in ('arena_quiescent','arena_can_free','arena_next_generation'):
        assert re.search(r'pub fnc '+name+r'\(', alloc)
    assert 'if count == 0 then return cursor; fi' in core
    assert 'if padding > limit - cursor then return -1; fi' in core
    assert 'if count > limit - aligned then return -1; fi' in core
    assert 'if admission_sealed == false || drops_complete == false then return false; fi' in alloc
    assert 'if generation >= maximum then return -1; fi' in alloc
    for mark in ('OwnedArena.create_bounded','OwnedArena.empty','on_drop(): void',
                 'self.storage.retire();','ArenaSlice','pub mtd (mut self: ScopedArena) allocate_zeroed'):
        assert mark in scoped
    assert 'import module limits from "atto.core/arena_limits";' in scoped
    assert 'throw ArenaAllocationError' in scoped
    assert 'pub fnc arena_near_quota' in std
    assert 'pub fnc arena_can_fit_request' in std
    assert not re.search(r'^import .*std', core + '\n' + alloc + '\n' + scoped, re.M)
    assert len(payload['cases']) == 38
    assert len(payload['negative_gates']) >= 15 and len(set(payload['negative_gates'])) == len(payload['negative_gates'])
    for v in payload['cases']:
        assert set(v) == {'fn','args','expected'} and v['fn'] in FUNCS
        assert FUNCS[v['fn']](*v['args']) == v['expected'],v
    return True


class ArenaSourceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.m = json.loads(CONTRACT.read_text())

    def test_checked_manifest_and_all_ledger_vectors(self):
        self.assertTrue(validate(self.m))

    def test_exhaustive_small_alignment_oracle(self):
        for cursor in range(24):
            for count in range(12):
                for align in (0,1,2,3,4,8,16):
                    for limit in (0,8,16,24):
                        got=arena_end_offset(cursor,count,align,limit)
                        self.assertTrue(got == -1 or (cursor <= got <= limit))
                        if count and got >= 0:
                            self.assertEqual((got-count) % align,0)

    def test_retirement_denied_for_every_outstanding_lease(self):
        self.assertTrue(arena_can_free(True,True,0,0,0,0,0))
        for i in range(5):
            counts=[0]*5
            counts[i]=1
            self.assertFalse(arena_can_free(True,True,*counts))
        self.assertFalse(arena_can_free(False,True,0,0,0,0,0))
        self.assertFalse(arena_can_free(True,False,0,0,0,0,0))

    def test_forged_guest_evidence_and_mutation_fail(self):
        for change in (
            lambda x: x['evidence'].__setitem__('guest_executed',True),
            lambda x: x['evidence']['backend_results'].__setitem__('llvm','PASS'),
            lambda x: x['source_paths'][0].__setitem__('bytes',1),
            lambda x: x['source_paths'][0].__setitem__('path','../../secret.atto'),
            lambda x: x['scope'].__setitem__('byte_identical_across_compilers_required',True),
            lambda x: x['cases'][0].__setitem__('expected',0),
            lambda x: x['cases'].pop(),
            lambda x: x.__setitem__('status','PASS'),
        ):
            m=copy.deepcopy(self.m)
            change(m)
            with self.assertRaises((AssertionError, FileNotFoundError)):
                validate(m)


if __name__ == '__main__':
    unittest.main()
