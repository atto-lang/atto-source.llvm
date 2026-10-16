"""Adversarial invalidation and snapshot tests for the experimental query engine."""

import random
import unittest

from experiments.incremental_query import QueryCycleError, QueryEngine
from test_incremental_query import demo


class HardenedQueryTests(unittest.TestCase):
    def test_input_and_result_mutation_cannot_change_cached_semantics(self):
        q = demo()
        calls = ["f"]
        q.set_input("calls:g", calls)
        original = q.result("typed", "g")
        counts = q.computations.copy()
        calls.append("phantom")
        self.assertEqual(q.result("typed", "g"), original)
        self.assertEqual(q.computations, counts)
        original.value["valid"] = False
        original.value["callee_interfaces"][0][1]["signature"] = "tampered"
        fresh = q.result("typed", "g")
        self.assertTrue(fresh.value["valid"])
        self.assertEqual(fresh.value["callee_interfaces"][0][1]["signature"], "fn(int) -> int")

    def test_absence_is_not_an_ordinary_json_dictionary(self):
        q = QueryEngine()
        missing = q.result("input", "x")
        q.set_input("x", {"absent": "x"})
        present = q.result("input", "x")
        self.assertNotEqual(missing.fingerprint, present.fingerprint)
        self.assertEqual(present.value, {"absent": "x"})

    def test_missing_required_inputs_and_contracts_fail_closed(self):
        for omitted in ("signature:f", "effects:f", "body:f", "calls:f"):
            q = QueryEngine()
            q.set_input("registry", ["f"])
            facts = {
                "signature:f": "fn() -> int",
                "effects:f": [],
                "body:f": "return 1",
                "calls:f": [],
            }
            for name, value in facts.items():
                if name != omitted:
                    q.set_input(name, value)
            self.assertFalse(q.result("typed", "f").value["valid"])
            with self.assertRaises(ValueError):
                q.result("object", "llvm\0f")

    def test_missing_toolchain_and_inlining_facts_rejected(self):
        q = QueryEngine()
        for name, value in {
            "registry": ["f"],
            "signature:f": "fn() -> int",
            "effects:f": [],
            "body:f": "return 1",
            "calls:f": [],
        }.items():
            q.set_input(name, value)
        with self.assertRaises(ValueError):
            q.result("object", "llvm\0f")
        q.set_input("inlines:f", [])
        with self.assertRaises(ValueError):
            q.result("object", "llvm\0f")
        q.set_input("toolchain:llvm", "pinned")
        self.assertEqual(q.result("object", "llvm\0f").value["target"], "llvm")

    def test_input_is_strict_bounded_json(self):
        q = demo()
        for value in ({1: "bad"}, {1: "a", "1": "b"}, (1, 2)):
            with self.assertRaises(TypeError):
                q.set_input("bad", value)
        nested = 0
        for _ in range(65):
            nested = [nested]
        with self.assertRaises(ValueError):
            q.set_input("deep", nested)
        with self.assertRaises(ValueError):
            q.set_input("large", ["123456789"] * 120000)
        with self.assertRaises(ValueError):
            q.set_input("", True)
        with self.assertRaises(AttributeError):
            q.schema_version = "mutable"
        with self.assertRaises(ValueError):
            QueryEngine(schema_version="")

    def test_effect_changes_invalidate_callers(self):
        q = demo()
        first = q.result("typed", "g").fingerprint
        q.set_input("effects:f", ["pure", "clock"])
        self.assertNotEqual(first, q.result("typed", "g").fingerprint)

    def test_cycles_leave_engine_reusable(self):
        class CyclicEngine(QueryEngine):
            def _compute(self, kind, identity):
                if kind == "cycle":
                    return self.use("cycle", identity)
                return super()._compute(kind, identity)
        q = CyclicEngine()
        with self.assertRaises(QueryCycleError):
            q.result("cycle", "f")
        q.set_input("live", True)
        self.assertTrue(q.result("input", "live").value)

    def test_invalid_object_identity_rejected(self):
        q = demo()
        with self.assertRaises(ValueError):
            q.result("object", "llvm\0f\0g")

    def test_random_edits_agree_with_fresh_reference(self):
        rng = random.Random(4107)
        q = demo()
        changes = {}
        for step in range(60):
            name = rng.choice(["body:f", "signature:f", "effects:f", "toolchain:llvm"])
            if name == "body:f":
                value = "return " + str(rng.randrange(10))
            elif name == "signature:f":
                value = rng.choice(["fn(int) -> int", "fn(int) -> string"])
            elif name == "effects:f":
                value = rng.choice([[], ["pure"], ["clock"]])
            else:
                value = "llvm@" + str(rng.randrange(4))
            q.set_input(name, value)
            changes[name] = value
            clean = demo()
            for key, item in changes.items():
                clean.set_input(key, item)
            self.assertEqual(q.result("link", "llvm").fingerprint,
                             clean.result("link", "llvm").fingerprint,
                             "Mismatch at edit " + str(step))
            self.assertEqual(q.result("typed", "g").fingerprint,
                             clean.result("typed", "g").fingerprint)


if __name__ == "__main__":
    unittest.main()
