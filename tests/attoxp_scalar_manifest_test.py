#!/usr/bin/env python3
"""Host-only corpus integrity; LLVM CTests separately compile and execute .atto guests."""
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1] / 'contracts' / 'attoxp-scalar-v1'
PINS = (
    ('answer-42.atto', 42, '08a415734e9992f47702dc9b1ccf2c5dceb74df05245eaa5fbb00512d18afc8a'),
    ('decrement-41.atto', 41, '90da332e7fb4e13443a7c6f741e03d872c8d0422359842f9291326c3c98e1fdf'),
    ('reset-zero.atto', 0, 'c26a72daa25e2d8da6ce0f5b8e1373c444db82537b74da9f687f43307277d6cf'),
    ('multiply-42.atto', 42, 'e07b8ff18f11896a087db1c608d5206df1e36832f5ae6f4b2f8cc505338e342f'),
)

def unique_dict(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f'duplicate JSON key: {key}')
        result[key] = value
    return result

def digest(data):
    return hashlib.sha256(data).hexdigest()

def main():
    manifest_path = ROOT / 'cases.json'
    assert not ROOT.is_symlink() and not manifest_path.is_symlink()
    manifest_bytes = manifest_path.read_bytes()
    assert 0 < len(manifest_bytes) <= 16 * 1024
    data = json.loads(manifest_bytes.decode('utf-8', errors='strict'), object_pairs_hook=unique_dict)
    assert data['schemaVersion'] == 1
    assert data['contract'] == 'attolang.attoxp.scalar-parity/1'
    assert data['truth'] == 'SCALAR_ONLY_NOT_UI_ABI_OR_ACTOR'
    assert len(data['cases']) == len(PINS)
    assert {p.name for p in ROOT.iterdir()} == {'cases.json'} | {name for name, _, _ in PINS}
    for row, (name, expected, sha) in zip(data['cases'], PINS, strict=True):
        assert row == {'id': name[:-5], 'file': name, 'expectedExit': expected, 'sha256': sha}
        file = ROOT / name
        assert file.is_file() and not file.is_symlink()
        raw = file.read_bytes()
        assert 0 < len(raw) <= 4096 and digest(raw) == sha
        assert digest(bytes([raw[0] ^ 1]) + raw[1:]) != sha, 'mutation must invalidate pin'
    try:
        json.loads('{"x":1,"x":2}', object_pairs_hook=unique_dict)
    except ValueError:
        pass
    else:
        raise AssertionError('duplicate key admitted')
    print('AttoXP four pinned LLVM/.NET/Wasm source fixtures: host hash gate PASS (NOT guest execution)')

if __name__ == '__main__':
    main()
