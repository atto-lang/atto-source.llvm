#!/usr/bin/env python3
"""Source-free regression tests for lifecycle IR gate; no Oreslang source or compiler needed."""
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
GOOD_IR = "; ModuleID = 'test'\n" + CALL + "\n" + CALL + "\n"

def result(code=0, out=GOOD_IR, err=""):
    return subprocess.CompletedProcess(["fake-llvmc"], code, out, err)

class LifecycleProbeTest(unittest.TestCase):
    def test_two_actual_bridge_calls_are_ir_only(self):
        message, valid = probe.evaluate(NAME, result())
        self.assertTrue(valid)
        self.assertIn("IR_ONLY_NEEDS_NATIVE_EXECUTION", message)

    def test_repeated_source_with_single_generated_await_is_gap(self):
        message, valid = probe.evaluate(NAME, result(out="; ModuleID = 'x'\n" + CALL + "\n"))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=1 required=2", message)

    def test_declaration_and_comment_are_not_guest_calls(self):
        text = ("; ModuleID = 'x'\n"
                "declare i64 @" + SYMBOL + "(ptr)\n"
                "; " + CALL.strip() + "\n" + CALL + "\n")
        message, valid = probe.evaluate(NAME, result(out=text))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=1", message)

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

    def test_extra_bridge_call_does_not_pass(self):
        message, valid = probe.evaluate(NAME, result(out=GOOD_IR + CALL + "\n"))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=3", message)

    def test_done_bridge_is_distinct(self):
        name = "source-actor-lifecycle-repeat-done-target.ores"
        done = "  call i64 @oreslang_source_actor_await_done(ptr %ref)"
        ready = "  call i64 @" + SYMBOL + "(ptr %ref)"
        message, valid = probe.evaluate(name, result(out="; ModuleID = 'x'\n" + done + "\n" + ready + "\n"))
        self.assertFalse(valid)
        self.assertIn("guest_bridge_calls=1", message)

if __name__ == "__main__":
    unittest.main()
