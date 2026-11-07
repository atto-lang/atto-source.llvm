"""Reference-only binder invariant probes for optional/named/rest call items.
This is NOT a frontend/parser/formatter/ownership or guest-execution test.
"""
import unittest

# Keep omitted optional slots distinct from an explicitly supplied typed None.
OMITTED = object()

def bind(formals, items):
    """formals: [(name, optional, is_rest)]; items: (kind,label,value,source_index)."""
    if any(rest for _, _, rest in formals[:-1]):
        raise ValueError("rest must be final")
    if sum(rest for _, _, rest in formals) > 1:
        raise ValueError("multiple rest parameters")
    names = [p[0] for p in formals]
    if len(set(names)) != len(names):
        raise ValueError("duplicate formal")
    fixed = len(formals) - int(bool(formals and formals[-1][2]))
    assigned = {}
    rest = []
    evaluation = []
    saw_named = False
    positional = 0
    for i, (kind, label, value, source_index) in enumerate(items):
        if source_index != i:
            raise ValueError("source item indexes must be contiguous")
        if kind not in ("value", "omit"):
            raise ValueError("unrecognized parsed call item")
        if label is not None:
            saw_named = True
            if label not in names:
                raise ValueError("unknown label")
            slot = names.index(label)
            if slot >= fixed:
                if kind == "omit" or assigned.get(slot) is not None or rest:
                    raise ValueError("invalid rest pack")
                if not isinstance(value, tuple):
                    raise ValueError("named rest requires typed pack representation")
                assigned[slot] = value
                evaluation.append(source_index)
                continue
        else:
            if saw_named:
                raise ValueError("positional after named")
            slot = positional
            positional += 1
        if slot >= fixed:
            if not formals or not formals[-1][2] or kind == "omit":
                raise ValueError("invalid extra/rest argument")
            rest.append(value)
            evaluation.append(source_index)
            continue
        if slot in assigned:
            raise ValueError("duplicate slot")
        if kind == "omit":
            if not formals[slot][1]:
                raise ValueError("required slot omitted")
            assigned[slot] = OMITTED
        else:
            assigned[slot] = value
            evaluation.append(source_index)
    for slot in range(fixed):
        if slot not in assigned:
            if not formals[slot][1]:
                raise ValueError("missing required")
            assigned[slot] = OMITTED
    if fixed < len(formals):
        assigned[fixed] = assigned.get(fixed, tuple(rest))
    return tuple(assigned[i] for i in range(len(formals))), tuple(evaluation)

class OptionalCallPlanReferenceTests(unittest.TestCase):
    def setUp(self):
        self.formals = [("a", False, False), ("b", True, False),
                        ("tags", False, True)]

    def test_origin_skips_omitted_item_but_preserves_lexical_evaluation(self):
        slots, source = bind(self.formals, [
            ("value", None, 3, 0), ("omit", None, None, 1),
            ("value", None, "effect-once", 2)])
        self.assertEqual(slots[0], 3)
        self.assertIs(slots[1], OMITTED)
        self.assertEqual(slots[2], ("effect-once",))
        self.assertEqual(source, (0, 2))

    def test_nontrailing_optional_with_named_reordering(self):
        slots, source = bind([("a", False, False), ("b", True, False),
                              ("c", False, False)], [
            ("value", "c", "third", 0),
            ("value", "a", "first", 1)])
        self.assertEqual(slots[0], "first")
        self.assertIs(slots[1], OMITTED)
        self.assertEqual(slots[2], "third")
        self.assertEqual(source, (0, 1))

    def test_named_rest_pack_preserves_original_item_origin(self):
        slots, source = bind(self.formals, [
            ("value", "tags", ("x", "y"), 0),
            ("value", "a", 5, 1)])
        self.assertEqual(slots[0], 5)
        self.assertIs(slots[1], OMITTED)
        self.assertEqual(slots[2], ("x", "y"))
        self.assertEqual(source, (0, 1))

    def test_no_rest_is_empty_tuple(self):
        slots, source = bind(self.formals, [("value", None, 3, 0)])
        self.assertEqual((slots[0], slots[2], source), (3, (), (0,)))
        self.assertIs(slots[1], OMITTED)

    def test_malformed_calls_fail_closed(self):
        bad = [
            [("omit", None, None, 0)],
            [("value", None, 1, 0), ("value", "a", 2, 1)],
            [("value", "b", "x", 0), ("value", None, 1, 1)],
            [("value", "unknown", 1, 0)],
            [("value", None, 1, 0), ("omit", None, None, 1),
             ("omit", None, None, 2)],
            [("value", None, 1, 1)],
        ]
        for items in bad:
            with self.subTest(items=items), self.assertRaises(ValueError):
                bind(self.formals, items)

    def test_explicit_none_is_still_an_evaluated_value(self):
        slots, source = bind(self.formals, [
            ("value", None, 3, 0),
            ("value", None, None, 1)])
        self.assertIsNone(slots[1])
        self.assertIsNot(slots[1], OMITTED)
        self.assertEqual(source, (0, 1))

    def test_nonfinal_rest_declaration_rejected(self):
        with self.assertRaisesRegex(ValueError, "rest must be final"):
            bind([("tags", False, True), ("a", False, False)], [])

if __name__ == "__main__":
    unittest.main()
