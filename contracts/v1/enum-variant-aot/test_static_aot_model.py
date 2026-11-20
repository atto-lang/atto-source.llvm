"""Independent, source-free finite oracle for Oreslang enum specialization AOT.

This is NOT executable Oreslang guest or compiler verification. It models the
static admission/selection/link invariants shared by Java and LLVM policies.
"""
from itertools import combinations, product
from types import MappingProxyType
import hashlib
import unittest

CASES = {"unit::Foo": ("A", "B", "C"), "unit::Bar": ("A", "D")}
TYPES = [("enum", "unit::Foo", None), ("enum", "unit::Bar", None)]
TYPES += [("case", owner, case) for owner, cases in CASES.items() for case in cases]
TYPES += [("primitive", "int", None), ("primitive", "bool", None)]


def atoms(t):
    kind, owner, case = t
    if kind == "primitive":
        return {(kind, owner)}
    if kind == "enum":
        return {(owner, c) for c in CASES[owner]}
    return {(owner, case)}


def domain(sig):
    return set(product(*(atoms(t) for t in sig)))


def admitted(a, b):
    # Non-enum type differences must not create separate same-arity slots.
    for x, y in zip(a, b):
        if x != y and (x[0] == "primitive" or y[0] == "primitive"):
            return False
    da, db = domain(a), domain(b)
    return da.isdisjoint(db) or da < db or db < da


def static_bind(family, args):
    actual = domain(args)
    matches = [x for x in family if actual <= domain(x)]
    minimal = [x for x in matches if all(domain(x) <= domain(y) for y in matches)]
    if len(minimal) != 1:
        raise ValueError("no unique statically proven call target")
    return minimal[0]


def seal_aot(family, compiled, kind="MODULE"):
    if kind not in ("MODULE", "STATIC"):
        raise ValueError("virtual instance slot cannot be direct-relocated")
    expected = {token(s, kind=kind) for s in family}
    if set(compiled) != expected:
        raise ValueError("missing or extra implementation")
    if any(len(digest) != 64 or any(ch not in "0123456789abcdef" for ch in digest)
           for digest in compiled.values()):
        raise ValueError("noncanonical implementation digest")
    return MappingProxyType(dict(compiled))


def token(signature, owner="file::module", kind="MODULE", name="f"):
    def field(x):
        data = x.encode("utf-8")
        return "|" + str(len(data)) + ":" + data.hex()
    value = "ores-enum-variant-slot-v1"
    for x in (owner, kind, name, str(len(signature))):
        value += field(x)
    for param in signature:
        ptype, family, case = param
        value += field("P" if ptype == "primitive" else "E" if ptype == "enum" else "V")
        value += field(family)
        if ptype == "case":
            value += field(case)
    return value


class StaticAotModelTest(unittest.TestCase):
    def test_all_3240_same_arity_pairs(self):
        signatures = list(product(TYPES, repeat=2))
        total = 0
        for a, b in combinations(signatures, 2):
            self.assertEqual(admitted(a, b), admitted(b, a))
            da, db = domain(a), domain(b)
            if admitted(a, b):
                self.assertTrue(da.isdisjoint(db) or da < db or db < da)
            total += 1
        self.assertEqual(total, 3240)

    def test_crossing_specializations_reject(self):
        whole = ("enum", "unit::Foo", None)
        case_a = ("case", "unit::Foo", "A")
        case_b = ("case", "unit::Foo", "B")
        self.assertFalse(admitted((case_a, whole), (whole, case_b)))
        self.assertTrue(admitted((whole, whole), (case_a, whole)))
        self.assertTrue(admitted((case_a, whole), (case_a, case_b)))

    def test_static_fallback_not_runtime_tag(self):
        whole = ("enum", "unit::Foo", None)
        case_a = ("case", "unit::Foo", "A")
        case_b = ("case", "unit::Foo", "B")
        family = ((whole,), (case_a,), (case_b,))
        self.assertEqual(static_bind(family, (case_a,)), (case_a,))
        self.assertEqual(static_bind(family, (whole,)), (whole,))
        with self.assertRaises(ValueError):
            static_bind(((case_a,), (case_b,)), (whole,))

    def test_symbol_encoding_parity_golden(self):
        # Separate Java/C++ golden fixture uses file::module and file::Foo.
        sig = (("case", "file::Foo", "A"), ("primitive", "int", None))
        expected = ("ores-enum-variant-slot-v1|12:66696c653a3a6d6f64756c65"
                    "|6:4d4f44554c45|1:66|1:32|1:56|9:66696c653a3a466f6f"
                    "|1:41|1:50|3:696e74")
        self.assertEqual(token(sig), expected)
        self.assertNotEqual(token(sig), token((("case", "file::Foo", "B"), sig[1])))

    def test_aot_direct_call_vs_virtual(self):
        whole = ("enum", "unit::Foo", None)
        case_a = ("case", "unit::Foo", "A")
        family = ((whole,), (case_a,))
        static_target = static_bind(family, (case_a,))
        digest = hashlib.sha256(b"compiled-body").hexdigest()
        exports = {token(item): digest for item in family}
        image = seal_aot(family, exports)
        self.assertEqual(set(image), {token(item) for item in family})
        self.assertEqual(image[token(static_target)], digest)
        # Module/static direct calls are allowed, but instance calls cannot
        # be direct-relocated without an independent final/sealed receiver proof.
        for kind in ("MODULE", "STATIC"):
            self.assertIn(token(static_target, kind=kind),
                          {token(item, kind=kind) for item in family})
        with self.assertRaises(ValueError):
            seal_aot(family, {token(item, kind="INSTANCE"): digest for item in family},
                     kind="INSTANCE")
        self.assertNotEqual(token(static_target, kind="INSTANCE"),
                            token(static_target, kind="MODULE"))

    def test_complete_closed_world_link_manifest(self):
        family = (("enum", "unit::Foo", None),), (("case", "unit::Foo", "A"),)
        defs = {token(sig): "a" * 64 for sig in family}
        self.assertEqual(len(defs), 2)
        self.assertEqual(set(defs), {token(x) for x in family})
        self.assertFalse(set(defs) == {token(family[0])})
        forged = dict(defs, **{"untrusted::other": "a" * 64})
        with self.assertRaises(ValueError):
            seal_aot(family, forged)
        with self.assertRaises(ValueError):
            seal_aot(family, {token(family[0]): "a" * 64})
        with self.assertRaises(ValueError):
            seal_aot(family, {token(sig): "B" * 64 for sig in family})
        image = seal_aot(family, defs)
        defs.clear()
        self.assertEqual(len(image), 2)


if __name__ == "__main__":
    unittest.main()
