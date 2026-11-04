"""Runner-independent regression checks for the exact-head LLVM AOT evidence gate."""
import importlib.util
import os
import subprocess
from pathlib import Path
import tempfile
import unittest

source = Path(__file__).with_name('emit_sealed_actor_receipt.py')
spec = importlib.util.spec_from_file_location('atto_aot_receipt', source)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

GOOD = '\n'.join([
    'define void @oreslang_actor_Worker_init(ptr %p, i64 %d, i64 %e) { ret void }',
    'define i64 @HANDLER(ptr %p, i64 %d, i64 %e, i64 %v) {',
    '  call void @ores_actor_i64_heap_scoped_new(ptr %p, i64 %d, i64 %e, i64 1, ptr %p)',
    '  call void @ores_actor_i64_heap_scoped_drop(ptr %p, i64 %d, i64 %e, ptr %p)',
    '  %r = call i64 @ores_actor_i64_heap_field_read(ptr %p, i64 %d, i64 %e, i64 0)',
    '  call void @ores_actor_i64_heap_field_write(ptr %p, i64 %d, i64 %e, i64 0, i64 1)',
    '  ret i64 1',
    '}',
])


class ReceiptTests(unittest.TestCase):
    def test_one_exact_receipt_only(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'receipt'
            for value in ('', module.LEGACY+'\n'+module.LEGACY, module.LEGACY+'\nextra', 'unrelated'):
                p.write_text(value)
                with self.assertRaises(ValueError):
                    module.check_output(p, module.LEGACY)
            p.write_text(module.LEGACY+'\n')
            module.check_output(p, module.LEGACY)

    def test_ir_requires_actual_definitions_calls_and_distinct_abi(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'guest.ll'
            for handler in ('oreslang_actor_Worker_on_message', 'oreslang_actor_Worker_handle_state'):
                good = GOOD.replace('HANDLER', handler)
                p.write_text(good)
                module.check_ir(p, handler)
                for bad in (
                    good.replace('define i64 @'+handler, 'declare i64 @'+handler),
                    good.replace('@ores_actor_i64_heap_scoped_drop(', '@missing_drop('),
                    good + '\ndefine i64 @'+('oreslang_actor_Worker_handle_state' if handler.endswith('on_message') else 'oreslang_actor_Worker_on_message')+'(ptr %p) { ret i64 0 }',
                ):
                    p.write_text(bad)
                    with self.assertRaises(ValueError):
                        module.check_ir(p, handler)

    def test_commented_out_ownership_calls_do_not_count(self):
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'guest.ll'
            good = GOOD.replace('HANDLER', 'oreslang_actor_Worker_handle_state')
            # A comment that looks like an LLVM instruction was previously
            # admitted by an unanchored 'call' regex.
            for bad in (
                good.replace('  call void @ores_actor_i64_heap_scoped_drop(',
                             '  ; call void @ores_actor_i64_heap_scoped_drop('),
                good.replace('  %r = call i64 @ores_actor_i64_heap_field_read(',
                             '  ; %r = call i64 @ores_actor_i64_heap_field_read('),
            ):
                p.write_text(bad)
                with self.assertRaises(ValueError):
                    module.check_ir(p, 'oreslang_actor_Worker_handle_state')

    def test_full_receipt_binds_clean_git_head_and_both_artifacts(self):
        # Uses a synthetic *local test repository*, not an AttoLang compiler
        # output or a certification receipt. Exercises the complete issuer.
        with tempfile.TemporaryDirectory() as td:
            workspace = Path(td)
            checkout = workspace / 'checkout'
            runner = workspace / 'runner'
            checkout.mkdir()
            runner.mkdir()
            subprocess.run(['git', 'init', '-q', str(checkout)], check=True)
            for name in module.SOURCES:
                p = checkout / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_text('fixture: ' + name + '\n')
            subprocess.run(['git', '-C', str(checkout), 'add', '.'], check=True)
            subprocess.run(['git', '-C', str(checkout), '-c', 'user.name=Test',
                            '-c', 'user.email=test@example.invalid', 'commit',
                            '-qm', 'isolated evidence fixture'], check=True)
            head = subprocess.check_output(['git', '-C', str(checkout),
                                            'rev-parse', 'HEAD'], text=True).strip()
            files = {
                'compiled-two-actor-owned-counter.ll': GOOD.replace('HANDLER',
                    'oreslang_actor_Worker_on_message'),
                'compiled-sealed-actor-counter.ll': GOOD.replace('HANDLER',
                    'oreslang_actor_Worker_handle_state'),
                'aot-two-actor-receipt': module.LEGACY + '\n',
                'aot-sealed-two-actor-receipt': module.SEALED + '\n',
            }
            for filename, content in files.items():
                (runner / filename).write_text(content)
            for name in ('guest.o', 'guest-sealed.o', 'aot-two-actor-test',
                         'aot-sealed-two-actor-test'):
                (runner / name).write_bytes(b'fixture not actual LLVM binary')
            env = {'GITHUB_RUN_ID': '123456', 'GITHUB_RUN_ATTEMPT': '2',
                   'GITHUB_REPOSITORY': 'private/source-test',
                   'GITHUB_JOB': 'compiled-native-actor-owned-heaps'}
            prior = Path.cwd()
            try:
                os.chdir(checkout)
                receipt = module.build_receipt(runner, 'asan-ubsan', head, env)
                self.assertEqual(receipt['head_sha'], head)
                self.assertTrue(receipt['compiled_guest_execution_verified'])
                self.assertTrue(receipt['asan_ubsan_mode'])
                self.assertEqual(set(receipt['executions']), {'legacy', 'sealed'})
                self.assertEqual(len(receipt['source_blobs']), len(module.SOURCES))
                self.assertFalse(receipt['full_state_intrinsic_parity_verified'])
                (checkout / module.SOURCES[0]).write_text('mutated source')
                with self.assertRaises(ValueError):
                    module.build_receipt(runner, 'optimized', head, env)
            finally:
                os.chdir(prior)

    def test_bad_head_and_mode_refused_before_git(self):
        with tempfile.TemporaryDirectory() as td:
            for mode, head in [('optimized', 'not-a-head'), ('fuzz', 'a'*40)]:
                with self.assertRaises(ValueError):
                    module.build_receipt(Path(td), mode, head, {})


if __name__ == '__main__':
    unittest.main()
