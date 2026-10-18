#!/usr/bin/env python3
"""Optional-parameter binding reference oracle; NOT an Oreslang runtime test."""
import json
from pathlib import Path

CASES = Path(__file__).with_name("optional-parameters-cases.json")

def bind(formals, actuals):
    if len({p["name"] for p in formals}) != len(formals):
        raise ValueError("duplicate declaration label")
    slots = {}
    lookup = {p["name"]: i for i, p in enumerate(formals)}
    named = False
    positional = 0
    for source_index, actual in enumerate(actuals):
        label = actual.get("label")
        if label is not None:
            named = True
            if label not in lookup:
                raise ValueError("unknown argument " + label)
            index = lookup[label]
        else:
            if named:
                raise ValueError("positional-after-named")
            if positional >= len(formals):
                raise ValueError("extra positional argument")
            index = positional
            positional += 1
        if index in slots:
            raise ValueError("duplicate argument " + formals[index]["name"])
        slots[index] = (actual["expr"], source_index)
    bound = []
    for index, param in enumerate(formals):
        if index not in slots:
            if not param.get("optional", False):
                raise ValueError("missing required argument " + param["name"])
            bound.append("None")
        else:
            expr, _ = slots[index]
            if param.get("optional", False) and expr != "None" and not expr.startswith("Some("):
                expr = "Some(" + expr + ")"
            bound.append(expr)
    return bound

def admit(signatures):
    ranges = [(sum(not item for item in signature), len(signature)) for signature in signatures]
    for i, (a, b) in enumerate(ranges):
        for x, y in ranges[i + 1:]:
            if max(a, x) <= min(b, y):
                return False
    return True

def main():
    cases = json.loads(CASES.read_text(encoding="utf-8"))
    formals = cases["parameters"]
    for test in cases["accepts"]:
        actual = bind(formals, test["args"])
        assert actual == test["slots"], (test["id"], actual, test["slots"])
    for test in cases["rejects"]:
        try:
            bind(formals, test["args"])
        except ValueError as exc:
            assert test["error"] in str(exc), (test["id"], str(exc))
        else:
            raise AssertionError("expected rejection: " + test["id"])
    for test in cases["overloads"]:
        actual = admit(test["signatures"])
        assert actual == test["admit"], (test["id"], actual)
    print("Optional-parameter reference oracle: %d accept, %d reject, %d overload cases passed" %
          (len(cases["accepts"]), len(cases["rejects"]), len(cases["overloads"])))

if __name__ == "__main__":
    main()
