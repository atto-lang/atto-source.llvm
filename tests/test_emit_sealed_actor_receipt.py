"""Runner-independent regression checks for the exact-head LLVM AOT evidence gate."""
import importlib.util
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

    def test_bad_head_and_mode_refused_before_git(self):
        with tempfile.TemporaryDirectory() as td:
            for mode, head in [('optimized', 'not-a-head'), ('fuzz', 'a'*40)]:
                with self.assertRaises(ValueError):
                    module.build_receipt(Path(td), mode, head, {})


if __name__ == '__main__':
    unittest.main()
