#!/usr/bin/env python3
"""Optional-parameter binding oracle, not native compiler or runtime conformance.

Value 'kind' annotations are a tiny stand-in for the eventual static type
checker. Never infer typed Option/None from string spellings or expressions.
"""
import json
from pathlib import Path

CASES = Path(__file__).with_name("optional-parameters-cases.json")


def check_formals(formals):
    labels = set()
    for index, formal in enumerate(formals):
        name = formal["name"]
        if not isinstance(name, str) or not name or name in labels:
            raise ValueError("duplicate/invalid declaration label")
        labels.add(name)
        if formal.get("rest", False):
            if formal.get("optional", False):
                raise ValueError("rest parameter cannot also be optional")
            if index != len(formals) - 1:
                raise ValueError("rest parameter must be final")
    return {p["name"]: i for i, p in enumerate(formals)}


def form_value(param, actual):
    """Represent the checked *shape* of an optional scalar value."""
    expr = actual["expr"]
    kind = actual.get("kind", "plain")
    if kind in ("null", "void"):
        raise ValueError("invalid-absence: null and void are not None")
    if kind not in ("plain", "option", "none"):
        raise ValueError("invalid value kind")
    if kind in ("none", "option"):
        if not param.get("optional", False):
            raise ValueError("Option value requires a compatible formal type")
        return expr
    return "Some(" + expr + ")" if param.get("optional", False) else expr


def bind(formals, actuals):
    lookup = check_formals(formals)
    slots = {}
    rest_sources = []
    named = False
    positional = 0
    rest_index = len(formals) - 1 if formals and formals[-1].get("rest", False) else None
    for source_index, actual in enumerate(actuals):
        if actual.get("spread", False):
            raise ValueError("spread requires a separate static expansion contract")
        label = actual.get("label")
        if label is not None:
            named = True
            if label not in lookup:
                raise ValueError("unknown argument " + str(label))
            index = lookup[label]
            if index == rest_index:
                raise ValueError("named rest requires explicit collection contract")
        else:
            if named:
                raise ValueError("positional-after-named")
            if rest_index is not None and positional >= rest_index:
                # Treat every positional item beyond the fixed prefix as one rest
                # element: not an Option, and never a runtime positional hole.
                if actual.get("kind", "plain") in ("null", "void"):
                    raise ValueError("invalid-absence in rest element")
                rest_sources.append((source_index, actual["expr"]))
                continue
            if positional >= len(formals):
                raise ValueError("extra positional argument")
            index = positional
            positional += 1
        if index in slots:
            raise ValueError("duplicate argument " + formals[index]["name"])
        slots[index] = (actual, source_index)

    bound = []
    for index, param in enumerate(formals):
        if index == rest_index:
            bound.append("Rest(" + ", ".join(expr for _, expr in rest_sources) + ")")
        elif index not in slots:
            if not param.get("optional", False):
                raise ValueError("missing required argument " + param["name"])
            bound.append("None")
        else:
            actual, _ = slots[index]
            bound.append(form_value(param, actual))
    source_indexes = [(-1 if i not in slots else slots[i][1])
                      for i in range(len(formals))]
    # Physical rest slot is one fixed ABI slot, regardless of call argument count.
    # Rest sources participate in the exactly-once permutation proof separately.
    used = [idx for idx in source_indexes if idx >= 0]
    used.extend(idx for idx, _ in rest_sources)
    if sorted(used) != list(range(len(actuals))):
        raise AssertionError("argument source evaluation was duplicated or dropped")
    return bound


def admit(signatures):
    """Conservative interval collision test (rest has unbounded upper arity)."""
    ranges = []
    for signature in signatures:
        flags = []
        for item in signature:
            if isinstance(item, bool):
                flags.append({"optional": item})
            elif isinstance(item, dict):
                flags.append(item)
            else:
                raise ValueError("invalid signature metadata")
        rest = bool(flags and flags[-1].get("rest", False))
        for index, param in enumerate(flags):
            if param.get("rest", False) and (index != len(flags) - 1
                                              or param.get("optional", False)):
                raise ValueError("invalid rest formal")
        minimum = sum(not p.get("optional", False) and not p.get("rest", False)
                      for p in flags)
        maximum = float("inf") if rest else len(flags)
        ranges.append((minimum, maximum))
    for i, (lo, hi) in enumerate(ranges):
        for other_lo, other_hi in ranges[i + 1:]:
            if max(lo, other_lo) <= min(hi, other_hi):
                return False
    return True


def main():
    cases = json.loads(CASES.read_text(encoding="utf-8"))
    formals = cases["parameters"]
    for test in cases["accepts"] + cases.get("rest_accepts", []):
        actual = bind(test.get("parameters", formals), test["args"])
        assert actual == test["slots"], (test["id"], actual, test["slots"])
    for test in cases["rejects"] + cases.get("rest_rejects", []):
        try:
            bind(test.get("parameters", formals), test["args"])
        except ValueError as exc:
            assert test["error"] in str(exc), (test["id"], str(exc))
        else:
            raise AssertionError("expected rejection: " + test["id"])
    for test in cases["overloads"] + cases.get("rest_overloads", []):
        actual = admit(test["signatures"])
        assert actual == test["admit"], (test["id"], actual)
    print("Optional-parameter reference oracle: %d accept, %d reject, %d overload cases passed" %
          (len(cases["accepts"]) + len(cases.get("rest_accepts", [])),
           len(cases["rejects"]) + len(cases.get("rest_rejects", [])),
           len(cases["overloads"]) + len(cases.get("rest_overloads", []))))


if __name__ == "__main__":
    main()
