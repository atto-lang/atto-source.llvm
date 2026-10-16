"""Deterministic differential stress test: incremental model vs clean recomputation."""

import random
import unittest

from experiments.incremental_query import QueryEngine
from test_incremental_query import demo


class QueryDifferentialStress(unittest.TestCase):
    def test_1000_mutations_across_nine_semantic_queries(self):
        rng = random.Random(424242)
        q = demo()
        edits = {}
        variants = {
            "registry": [["f", "g"], ["g", "f"], ["f"], ["f", "g", "h"]],
            "body:f": ["a", "b", "return f(3)"],
            "body:g": ["call f", "return 20"],
            "signature:f": ["int -> int", "int -> void", "f<T>"],
            "effects:f": [[], ["pure"], ["io"]],
            "calls:g": [[], ["f"], ["h"]],
            "inlines:g": [[], ["f"], ["h"]],
            "toolchain:llvm": ["llvm-A", "llvm-B"],
            "toolchain:wasm": ["wasm-A", "wasm-B"],
        }
        queries = [
            ("typed", "f"), ("typed", "g"), ("mir", "f"),
            ("interface", "f"), ("interface", "h"),
            ("object", "llvm\0f"), ("object", "llvm\0g"),
            ("link", "llvm"), ("link", "wasm"),
        ]

        def observe(engine: QueryEngine, kind: str, symbol: str):
            try:
                return ("ok", engine.result(kind, symbol).fingerprint)
            except (ValueError, TypeError) as error:
                return ("error", type(error).__name__, str(error))

        for step in range(1_000):
            name = rng.choice(list(variants))
            value = rng.choice(variants[name])
            q.set_input(name, value)
            edits[name] = value
            clean = demo()
            for key, item in edits.items():
                clean.set_input(key, item)
            for kind, symbol in queries:
                self.assertEqual(
                    observe(q, kind, symbol), observe(clean, kind, symbol),
                    f"Mismatch at edit {step}, query {(kind, symbol)}",
                )


if __name__ == "__main__":
    unittest.main()
