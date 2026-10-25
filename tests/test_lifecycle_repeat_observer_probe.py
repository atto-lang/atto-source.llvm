#!/usr/bin/env python3
"""Source-free exact-shape tests for the single coalesced lifecycle continuation."""
import importlib.util
import pathlib
import subprocess
import unittest

MODULE = pathlib.Path(__file__).with_name("lifecycle_repeat_observer_parity_probe.py")
spec = importlib.util.spec_from_file_location("lifecycle_probe_under_test", MODULE)
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)

NAME = "source-actor-lifecycle-repeat-ready-target.ores"
SYMBOL = "oreslang_source_actor_await_ready"
CALL = "  %status = call i64 @" + SYMBOL + "(ptr %ref)"
GOOD_IR = "; ModuleID = 'test'\n" + CALL + "\n"

def result(code=0, out=GOOD_IR, err=""):
    return subprocess.CompletedProcess(["fake-llvmc"], code, out, err)

class LifecycleProbeTest(unittest.TestCase):
    def test_single_coalesced_bridge_call_is_ir_only(self):
        message, valid = probe.evaluate(NAME, result())
        self.assertTrue(valid)
        self.assertIn("IR_ONLY_NEEDS_NATIVE_EXECUTION", message)

    def test_second_compiled_continuation_is_gap_not_required(self):
        message, valid = probe.evaluate(NAME, result(out=GOOD_IR + CALL + "\n"))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=2 required=1", message)

    def test_declaration_and_comment_are_not_executable_calls(self):
        text = ("; ModuleID = 'x'\n"
                "declare i64 @" + SYMBOL + "(ptr)\n"
                "; " + CALL.strip() + "\n")
        message, valid = probe.evaluate(NAME, result(out=text))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=0", message)

    def test_one_real_call_beside_declaration_still_coalesces(self):
        text = ("; ModuleID = 'x'\n" + "declare i64 @" + SYMBOL + "(ptr)\n"
                + "; " + CALL.strip() + "\n" + CALL + "\n")
        message, valid = probe.evaluate(NAME, result(out=text))
        self.assertTrue(valid)

    def test_nonzero_compiler_status_is_open_gap(self):
        message, valid = probe.evaluate(NAME, result(code=2, out="", err="unsupported await"))
        self.assertFalse(valid)
        self.assertIn("OPEN_GAP", message)

    def test_partial_ir_on_failure_is_hard_error(self):
        with self.assertRaisesRegex(RuntimeError, "leaked partial LLVM IR"):
            probe.evaluate(NAME, result(code=2, out="; ModuleID = 'leak'"))

    def test_bogus_compiler_success_is_hard_error(self):
        with self.assertRaisesRegex(RuntimeError, "without LLVM IR"):
            probe.evaluate(NAME, result(out="not LLVM IR"))

    def test_ready_call_is_not_done_call(self):
        name = "source-actor-lifecycle-repeat-done-target.ores"
        message, valid = probe.evaluate(name, result(out=GOOD_IR))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=0", message)

    def test_done_one_coalesced_bridge_call(self):
        name = "source-actor-lifecycle-repeat-done-target.ores"
        ir = "; ModuleID = 'x'\n  call i64 @oreslang_source_actor_await_done(ptr %ref)\n"
        message, valid = probe.evaluate(name, result(out=ir))
        self.assertTrue(valid)
        self.assertIn("IR_ONLY_NEEDS_NATIVE_EXECUTION", message)

    def test_duplicated_done_bridge_call_is_gap(self):
        name = "source-actor-lifecycle-repeat-done-target.ores"
        call = "  call i64 @oreslang_source_actor_await_done(ptr %ref)\n"
        message, valid = probe.evaluate(name, result(out="; ModuleID = 'x'\n"+call+call))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=2", message)

if __name__ == "__main__":
    unittest.main()
