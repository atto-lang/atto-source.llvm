#!/usr/bin/env python3
"""Source-free adversarial tests for exact native actor owned-token IR evidence.

Synthetic LLVM IR is NOT real Oreslang source-compilation evidence.
"""
import importlib.util
import pathlib
import unittest

harness = pathlib.Path(__file__).with_name("source_actor_e2e.py")
spec = importlib.util.spec_from_file_location("source_actor_e2e", harness)
assert spec and spec.loader
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
verify = module.verify_native_owned_ir

PREVIOUS = "%previous.actor.owned.token"
DOUBLED = "%doubled.actor.owned.token"
NEW = "ores_actor_i64_heap_scoped_new"
DROP = "ores_actor_i64_heap_scoped_drop"


def stack_token(name, words=3):
    return f"  {name} = alloca [{words} x i64], align 8"


def call(kind, token):
    return f"  call void @{kind}(ptr %heap, i64 %domain, i64 %epoch, ptr {token})"


def good():
    return "\n".join([
        "; ModuleID = 'owned-actor'",
        "define i64 @actor_handler(ptr %heap, i64 %domain, i64 %epoch) {",
        "entry:",
        stack_token(PREVIOUS),
        call(NEW, PREVIOUS),
        stack_token(DOUBLED),
        call(NEW, DOUBLED),
        call(DROP, DOUBLED),
        call(DROP, PREVIOUS),
        "  ret i64 3",
        "}",
    ])


class NativeOwnedIrVerifierTest(unittest.TestCase):
    def test_exact_lifo_owned_ir(self):
        verify(good(), "native-owner-good")

    def test_incidental_i64_3_does_not_prove_owner_tokens(self):
        with self.assertRaises(AssertionError):
            verify("define i64 @f(){\n  ret i64 3\n}", "forge")

    def test_wrong_stack_reserve_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace("alloca [3 x i64]", "alloca [2 x i64]"), "forge")

    def test_missing_native_constructor_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(NEW, PREVIOUS), ""), "forge")

    def test_missing_native_drop_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(DROP, DOUBLED), ""), "forge")

    def test_repeated_native_drop_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(DROP, DOUBLED), call(DROP, DOUBLED) + "\n" + call(DROP, DOUBLED)), "forge")

    def test_drop_order_must_be_lifo(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(DROP, DOUBLED) + "\n" + call(DROP, PREVIOUS),
                                  call(DROP, PREVIOUS) + "\n" + call(DROP, DOUBLED)), "forge")

    def test_wrong_drop_token_pointer_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(DROP, PREVIOUS), call(DROP, "%foreign.token")), "forge")

    def test_comment_only_call_is_denied(self):
        forged = good().replace(call(DROP, DOUBLED), "  ; " + call(DROP, DOUBLED).strip())
        with self.assertRaises(AssertionError):
            verify(forged, "forge")

    def test_non_owned_three_word_stack_slot_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace("previous.actor.owned.token", "anonymous.unowned.slot"), "forge")

    def test_drop_before_constructor_is_denied(self):
        altered = good().replace(
            call(NEW, DOUBLED) + "\n" + call(DROP, DOUBLED),
            call(DROP, DOUBLED) + "\n" + call(NEW, DOUBLED))
        with self.assertRaises(AssertionError):
            verify(altered, "premature-free")

    def test_cross_function_token_replay_is_denied(self):
        altered = good().replace(
            call(DROP, PREVIOUS),
            "}\ndefine void @not_actor(ptr %heap) {\nentry:\n" +
            call(DROP, PREVIOUS))
        with self.assertRaises(AssertionError):
            verify(altered, "cross-function-drop")

    def test_duplicate_allocation_without_constructor_is_denied(self):
        altered = good().replace(
            stack_token(DOUBLED),
            stack_token(DOUBLED) + "\n" + stack_token("%phantom.actor.owned.token"))
        with self.assertRaises(AssertionError):
            verify(altered, "phantom")

    def test_duplicate_constructor_is_denied(self):
        with self.assertRaises(AssertionError):
            verify(good().replace(call(NEW, PREVIOUS), call(NEW, PREVIOUS) + "\n" + call(NEW, PREVIOUS)), "forge")


if __name__ == "__main__":
    unittest.main()
