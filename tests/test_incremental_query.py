"""Executable red/green invalidation regression tests for the reference prototype."""

import math
import unittest

from experiments.incremental_query import QueryEngine


def demo() -> QueryEngine:
    q = QueryEngine()
    q.set_input("registry", ["f", "g"])
    for name, calls in (("f", []), ("g", ["f"])):
        q.set_input("signature:" + name, "fn(int) -> int")
        q.set_input("effects:" + name, ["pure"])
        q.set_input("body:" + name, "return 1" if name == "f" else "return f(1)")
        q.set_input("calls:" + name, calls)
        q.set_input("inlines:" + name, [])
    q.set_input("toolchain:llvm", "llvm@pinned")
    q.set_input("toolchain:wasm", "wasm@pinned")
    return q


class IncrementalQueryTests(unittest.TestCase):
    def test_noop_and_identical_input_are_fully_green(self):
        q = demo()
        first = q.result("link", "llvm").fingerprint
        counts = q.computations.copy()
        self.assertFalse(q.set_input("body:f", "return 1"))
        self.assertEqual(q.result("link", "llvm").fingerprint, first)
        self.assertEqual(q.computations, counts)

    def test_body_edit_does_not_retypecheck_noninlined_callers(self):
        q = demo()
        q.result("link", "llvm")
        typed_g = q.result("typed", "g").fingerprint
        old = q.computations.copy()
        q.set_input("body:f", "return 3")
        q.result("link", "llvm")
        self.assertEqual(q.result("typed", "g").fingerprint, typed_g)
        self.assertEqual(q.computations[("typed", "g")], old[("typed", "g")])
        self.assertEqual(q.computations[("object", "llvm\0g")], old[("object", "llvm\0g")])
        self.assertGreater(q.computations[("object", "llvm\0f")], old[("object", "llvm\0f")])
        self.assertGreater(q.computations[("link", "llvm")], old[("link", "llvm")])

    def test_signature_edit_invalidates_typed_dependents(self):
        q = demo()
        q.result("link", "llvm")
        old = q.computations.copy()
        q.set_input("signature:f", "fn(int) -> string")
        q.result("link", "llvm")
        self.assertGreater(q.computations[("interface", "f")], old[("interface", "f")])
        self.assertGreater(q.computations[("typed", "g")], old[("typed", "g")])
        self.assertGreater(q.computations[("object", "llvm\0g")], old[("object", "llvm\0g")])

    def test_inlining_invalidates_codegen_but_not_caller_type(self):
        q = demo()
        q.set_input("inlines:g", ["f"])
        q.result("link", "llvm")
        old = q.computations.copy()
        q.set_input("body:f", "return 5")
        q.result("link", "llvm")
        self.assertEqual(q.computations[("typed", "g")], old[("typed", "g")])
        self.assertGreater(q.computations[("object", "llvm\0g")], old[("object", "llvm\0g")])

    def test_negative_resolution_dependency_recovers_when_symbol_appears(self):
        q = demo()
        q.set_input("calls:g", ["later"])
        first = q.result("typed", "g")
        self.assertFalse(first.value["valid"])
        q.set_input("registry", ["f", "g", "later"])
        q.set_input("signature:later", "fn() -> int")
        q.set_input("effects:later", [])
        q.set_input("body:later", "return 6")
        q.set_input("calls:later", [])
        q.set_input("inlines:later", [])
        second = q.result("typed", "g")
        self.assertTrue(second.value["valid"])
        self.assertNotEqual(first.fingerprint, second.fingerprint)

    def test_target_specific_changes_reuse_target_neutral_analysis(self):
        q = demo()
        q.result("link", "llvm")
        q.result("link", "wasm")
        old = q.computations.copy()
        q.set_input("toolchain:wasm", "wasm@new")
        q.result("link", "wasm")
        q.result("link", "llvm")
        self.assertEqual(q.computations[("typed", "f")], old[("typed", "f")])
        self.assertEqual(q.computations[("typed", "g")], old[("typed", "g")])
        self.assertEqual(q.computations[("object", "llvm\0f")], old[("object", "llvm\0f")])
        self.assertGreater(q.computations[("object", "wasm\0f")], old[("object", "wasm\0f")])

    def test_untrusted_nonfinite_or_unserializable_inputs_rejected(self):
        q = demo()
        with self.assertRaises(ValueError):
            q.set_input("bad", math.nan)
        with self.assertRaises(TypeError):
            q.set_input("bad", object())


if __name__ == "__main__":
    unittest.main()
