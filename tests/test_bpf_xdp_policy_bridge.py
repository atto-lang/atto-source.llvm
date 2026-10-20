#!/usr/bin/env python3
"""Source-free mocks of the bridge's control flow; NOT LLVM/kernel evidence."""
import hashlib
import sys
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch

from bpf_xdp_policy_bridge import (
    Denied, PACKET, build, check_bpf_elf, digest, pinned_bytes,
    strict_json, validate_closure, verify_object_metadata, run_command,
    policy_bundle_hash, POLICY_MODULES, POLICY_PROFILES,
)


def closure(effects=()):
    return dict(schemaVersion=1, entry="ores_xdp", target="ebpf", backend="llvm",
                grants=[], nodes=[dict(id="ores_xdp", targets=["ebpf"],
                                      requires=list(effects), imports=[])])


def elf():
    raw = bytearray(64)
    raw[:6] = b"\x7fELF\x02\x01"
    struct.pack_into("<HHI", raw, 16, 1, 247, 1)
    return bytes(raw)


def make_policy_bundle(folder):
    root = folder / 'target-profiles'
    for name in POLICY_MODULES + tuple('profiles/' + x + '.json'
                                       for x in POLICY_PROFILES):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(('mock: ' + name).encode())
    return root / 'bin' / 'oreslang-target.mjs'


class AdmissionTests(unittest.TestCase):
    def test_both_valid_closure_shapes(self):
        self.assertFalse(validate_closure(closure()))
        self.assertTrue(validate_closure(closure([PACKET])))

    def test_corrupt_closures_fail_closed(self):
        mutations = [
            {**closure(), "grants": [PACKET]},
            {**closure(), "target": "server"},
            {**closure(), "backend": "graal"},
            {**closure(), "schemaVersion": True},
            {**closure(), "unknown": 123},
            {**closure(), "nodes": []},
            {**closure(), "nodes": [{**closure()['nodes'][0], "imports": ['x']}]},
            {**closure(), "nodes": [{**closure()['nodes'][0], "requires": ["heap"]}]},
            {**closure(), "nodes": [{**closure()['nodes'][0], "requires": [PACKET, PACKET]}]},
            {**closure(), "nodes": [{**closure()['nodes'][0], "requires": PACKET}]},
        ]
        for item in mutations:
            with self.subTest(item=item), self.assertRaises(Denied):
                validate_closure(item)

    def test_duplicate_or_nonfinite_json_rejected(self):
        bad = [
            b'{"target":"ebpf","target":"server"}',
            b'{"target":"ebpf","tar\\u0067et":"server"}',
            b'{"x":NaN}', b'{"x":Infinity}', b'{"x":-Infinity}',
            b'\xff', b'\xef\xbb\xbf{}',
        ]
        for text in bad:
            with self.subTest(text=text), self.assertRaises(Denied):
                strict_json(text)

    def test_pinned_regular_file_symlink_and_size(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'program.ores'
            path.write_bytes(b'hello')
            self.assertEqual(pinned_bytes(path, digest(b'hello')), b'hello')
            with self.assertRaises(Denied):
                pinned_bytes(path, digest(b'another'))
            alias = Path(tmp) / 'alias.ores'
            alias.symlink_to(path)
            with self.assertRaises(OSError):
                pinned_bytes(alias, digest(b'hello'))
            path.write_bytes(b'x' * 4097)
            with self.assertRaises(Denied):
                pinned_bytes(path, digest(b'x' * 4097))

    def test_reject_non_bpf_elf(self):
        for bad in (b'', b'ELF', elf()[:32], elf()[:18]+b'\x00\x00'+elf()[20:]):
            with self.subTest(blob=bad), self.assertRaises(Denied):
                check_bpf_elf(bad)
        check_bpf_elf(elf())

    def test_symbol_relocation_section_gates(self):
        section = '  [ 1] xdp PROGBITS 0000000000000000 000040 000010 00 AX 0 0 8\n' + \
                  '  [ 2] license PROGBITS 0000000000000000 000050 00000c 00 A 0 0 1\n'
        symbols = '  1: 0000 0 NOTYPE GLOBAL DEFAULT 1 ores_xdp\n' + \
                  '  2: 0000 0 OBJECT GLOBAL DEFAULT 2 _license\n'
        def good(argv, expected=0, timeout=90):
            opt = argv[2]
            return (section if opt == '-S' else symbols if opt == '-s'
                    else 'There are no relocations in this file.').encode()
        with patch('bpf_xdp_policy_bridge.run_command', side_effect=good):
            verify_object_metadata('/tmp/mock.o', 'readelf')
        def bad(argv, expected=0, timeout=90):
            if argv[2] == '-S':
                return section.replace('license PROGBITS', '.maps PROGBITS').encode()
            return good(argv, expected, timeout)
        with patch('bpf_xdp_policy_bridge.run_command', side_effect=bad):
            with self.assertRaises(Denied):
                verify_object_metadata('/tmp/mock.o', 'readelf')

    def test_build_mocked_full_happy_paths_and_denials(self):
        # Source-free tool stubs exercise fixed invocation order and deny-before-IR.
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            source=folder/'xdp.ores'; source.write_bytes(b'pub fnc ores_xdp(): int { return 2; }\n')
            compiler=folder/'compiler'; compiler.write_bytes(b'fake compiler')
            policy=make_policy_bundle(folder)
            class Args: pass
            args=Args(); args.source=source; args.compiler=compiler; args.policy_cli=policy
            args.expected_source_sha256=digest(source.read_bytes())
            args.expected_compiler_sha256=digest(compiler.read_bytes())
            args.expected_policy_cli_sha256=digest(policy.read_bytes())
            args.expected_policy_bundle_sha256=policy_bundle_hash(policy)
            args.node='node'; args.clang='clang'; args.readelf='readelf'
            for effects in ([],[PACKET]):
                args.grants=PACKET if effects else ''
                calls=[]
                def fake(argv, expected=0, timeout=90):
                    calls.append(argv)
                    if '--emit-target-closure' in argv:
                        return json.dumps(closure(effects)).encode()
                    if argv[0] == args.node:
                        self.assertIn('--grants='+args.grants, argv)
                        return b'OK'
                    if argv[0] == 'clang':
                        Path(argv[-1]).write_bytes(elf())
                        return b''
                    return b'target triple = "bpfel"\nsection "xdp"\nsection "license"\n'
                with patch('bpf_xdp_policy_bridge.run_command', side_effect=fake), \
                     patch('bpf_xdp_policy_bridge.verify_object_metadata'):
                    receipt=build(args)
                self.assertFalse(receipt['verifiedByKernel'])
                self.assertEqual(receipt['inferredEffects'], effects)
                self.assertEqual(receipt['bpfElfSha256'],digest(elf()))
                self.assertEqual(len(calls), 4)
                self.assertEqual(calls[1][0],'node')
                self.assertEqual(calls[2][0],str(compiler))
                self.assertEqual(calls[3][0],'clang')
            # A policy-grant mismatch denies without compiling any LLVM IR.
            args.grants=''
            calls=[]
            def fake(argv, **kwargs):
                calls.append(argv)
                return json.dumps(closure([PACKET])).encode()
            with patch('bpf_xdp_policy_bridge.run_command', side_effect=fake), \
                 self.assertRaises(Denied):
                build(args)
            self.assertEqual(len(calls),1)


    def test_policy_bundle_pins_all_imported_modules_and_profiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root=Path(tmp)
            cli=make_policy_bundle(root)
            observed=policy_bundle_hash(cli)
            self.assertEqual(observed,policy_bundle_hash(cli))
            module=cli.parent.parent/'src'/'contract.mjs'
            module.write_bytes(b'modified authority contract')
            self.assertNotEqual(policy_bundle_hash(cli),observed)
            module.unlink()
            module.symlink_to(cli)
            with self.assertRaises(Denied):
                policy_bundle_hash(cli)
            module.unlink()
            module.write_bytes(b'restored')
            extra=cli.parent.parent/'profiles'/'evil.json'
            extra.write_bytes(b'{}')
            with self.assertRaises(Denied):
                policy_bundle_hash(cli)

    def test_external_output_is_bounded_before_reading(self):
        with self.assertRaisesRegex(Denied, 'exceeds bounded diagnostic budget'):
            run_command([sys.executable, '-c', 'print("x" * 300000)'])
        self.assertEqual(run_command([sys.executable, '-c', 'print("OK")']), b'OK\n')

    def test_policy_checker_denial_prevents_ir_and_object_emission(self):
        with tempfile.TemporaryDirectory() as tmp:
            folder=Path(tmp)
            source=folder/'xdp.ores'; source.write_bytes(b'pub fnc ores_xdp(): int { return 2; }\n')
            compiler=folder/'compiler'; compiler.write_bytes(b'compiler')
            policy=make_policy_bundle(folder)
            class Args: pass
            args=Args(); args.source=source; args.compiler=compiler; args.policy_cli=policy
            args.expected_source_sha256=digest(source.read_bytes())
            args.expected_compiler_sha256=digest(compiler.read_bytes())
            args.expected_policy_cli_sha256=digest(policy.read_bytes())
            args.expected_policy_bundle_sha256=policy_bundle_hash(policy)
            args.grants=''; args.node='node'; args.clang='clang'; args.readelf='readelf'
            calls=[]
            def reject_checker(argv, expected=0, timeout=90):
                calls.append(argv)
                if '--emit-target-closure' in argv:
                    return json.dumps(closure()).encode()
                raise Denied('policy checker rejected')
            with patch('bpf_xdp_policy_bridge.run_command', side_effect=reject_checker):
                with self.assertRaisesRegex(Denied, 'policy checker rejected'):
                    build(args)
            self.assertEqual(len(calls), 2)
            self.assertEqual(calls[1][0], 'node')
            args.expected_compiler_sha256=digest(b'not compiler')
            calls.clear()
            with patch('bpf_xdp_policy_bridge.run_command', side_effect=reject_checker):
                with self.assertRaisesRegex(Denied, 'compiler executable SHA256'):
                    build(args)
            self.assertEqual(calls, [])
            args.expected_compiler_sha256=digest(compiler.read_bytes())
            args.expected_policy_cli_sha256=digest(b'not policy')
            with patch('bpf_xdp_policy_bridge.run_command', side_effect=reject_checker):
                with self.assertRaisesRegex(Denied, 'target-policy CLI SHA256'):
                    build(args)
            self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
