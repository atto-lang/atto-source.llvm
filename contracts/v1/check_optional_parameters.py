#!/usr/bin/env python3
"""Reference semantics for optional parameters and variadic tails.

This is not an Oreslang parser, type checker or runtime test. Argument types
are supplied by test metadata, NEVER inferred from the spelling of Some(...).
"""
import json
from pathlib import Path

CASES = Path(__file__).with_name("optional-parameters-cases.json")


def validate(formals):
    names = set()
    rest = False
    for index, formal in enumerate(formals):
        name = formal["name"]
        if name in names:
            raise ValueError("duplicate declaration label")
        names.add(name)
        if formal.get("rest", False):
            if formal.get("optional", False):
                raise ValueError("rest cannot be optional")
            if index != len(formals) - 1 or rest:
                raise ValueError("rest must be final and unique")
            rest = True
    return rest


def check_value(item, formal):
    expr = item["expr"]
    value_type = item["value_type"]
    typ = formal["type"]
    expected_option = "Option<" + typ + ">"
    if expr == "void" or value_type == "void":
        raise ValueError("void is not an argument value")
    if expr == "null" or value_type == "null":
        raise ValueError("null is forbidden; use Option")
    if expr == "omit":
        raise ValueError("omit must be a tagged omission item")
    # Type metadata is supplied by the fixture, not a parser. Still reject
    # claims that the intrinsic None expression has a non-Option type; never
    # turn forged "string None" into Some(None).
    if expr == "None" and not (
            value_type.startswith("Option<") and value_type.endswith(">")):
        raise ValueError("None requires a typed Option value")
    if formal.get("optional", False):
        if value_type == expected_option:
            return expr, "explicit_none" if expr == "None" else "provided"
        if value_type == typ:
            return "Some(" + expr + ")", "provided"
        raise ValueError("type mismatch: optional " + typ + " versus " + value_type)
    if value_type != typ:
        raise ValueError("type mismatch: required " + typ + " versus " + value_type)
    return expr, "explicit_none" if expr == "None" else "provided"


def check_pack(item, formal):
    kind = item.get("kind")
    if kind not in ("spread", "pack"):
        raise ValueError("rest must use a pack or variadic positional element")
    typ = formal["type"]
    # An absence keyword is not a sequence, even when a synthetic fixture
    # dishonestly assigns it a collection type.
    if item.get("expr") in ("None", "null", "void", "omit", "_"):
        raise ValueError("rest pack requires a real collection expression")
    accepted = {"List<" + typ + ">", "Array<" + typ + ">", "Slice<" + typ + ">"}
    if item.get("value_type") not in accepted:
        raise ValueError("type mismatch: rest pack expected " + typ)
    return "..." + item["expr"]


def bind(formals, actuals):
    has_rest = validate(formals)
    fixed = formals[:-1] if has_rest else formals
    lookup = {p["name"]: i for i, p in enumerate(formals)}
    bound = {}
    rest_items = []
    rest_mode = None
    seen_named = False
    positional = 0
    order = []
    evaluated_call_item_indices = []
    explicit_omission_bindings = []

    for source_index, item in enumerate(actuals):
        kind = item.get("kind", "value")
        label = item.get("label")
        # The lexer/parser must emit an Omit item for a standalone '_'.
        # '_' is not an expression or a partial-application placeholder.
        if kind == "omit" and ("expr" in item or "value_type" in item):
            raise ValueError("omit marker cannot carry a value")
        if kind == "underscore" and "value_type" in item:
            raise ValueError("underscore omission cannot carry a typed value")
        if kind == "underscore":
            if item.get("expr") not in (None, "_"):
                raise ValueError("malformed underscore omission marker")
            kind = "omit"
        if kind == "value" and item.get("expr") == "_":
            raise ValueError("underscore is a call-item omission, not a value")
        if kind not in ("value", "omit", "spread", "pack"):
            raise ValueError("invalid call argument kind")
        if kind == "spread" and label is not None:
            raise ValueError("named spread is not a named rest pack")
        if kind == "pack" and label is None:
            raise ValueError("rest pack must be named")
        if label is not None:
            seen_named = True
            if label not in lookup:
                raise ValueError("unknown argument " + label)
            index = lookup[label]
        else:
            if seen_named:
                raise ValueError("positional-after-named")
            if kind == "pack":
                raise ValueError("rest pack must be named")
            index = positional
            positional += 1
            if index >= len(fixed) and not has_rest:
                raise ValueError("extra positional argument")

        if has_rest and index >= len(fixed):
            if kind == "omit":
                raise ValueError("rest cannot be omitted")
            if label is not None:
                if rest_mode is not None:
                    raise ValueError("duplicate rest argument")
                rest_items.append(check_pack(item, formals[-1]))
                rest_mode = "rest_pack"
                order.append(item["expr"])
                evaluated_call_item_indices.append(source_index)
            elif kind == "spread":
                if rest_mode is not None:
                    raise ValueError("rest spread must be final and unique")
                if source_index != len(actuals) - 1:
                    raise ValueError("rest spread must be final")
                rest_items.append(check_pack(item, formals[-1]))
                rest_mode = "rest_spread"
                order.append(item["expr"])
                evaluated_call_item_indices.append(source_index)
            else:
                if rest_mode in ("rest_pack", "rest_spread"):
                    raise ValueError("rest spread must be final")
                if item["value_type"] != formals[-1]["type"]:
                    raise ValueError("type mismatch in variadic element")
                rest_items.append(item["expr"])
                rest_mode = "rest_values"
                order.append(item["expr"])
                evaluated_call_item_indices.append(source_index)
            continue

        if index >= len(fixed):
            raise ValueError("extra positional argument")
        if index in bound:
            raise ValueError("duplicate argument " + fixed[index]["name"])
        param = fixed[index]

        if kind == "omit":
            if not param.get("optional", False):
                raise ValueError("required parameter cannot be omitted: " + param["name"])
            bound[index] = ("None", "explicit_omit")
            explicit_omission_bindings.append([source_index, index])
        elif kind in ("spread", "pack"):
            raise ValueError("fixed-arity runtime spread or rest pack is unsupported")
        else:
            bound[index] = check_value(item, param)
            order.append(item["expr"])
            evaluated_call_item_indices.append(source_index)

    slots, statuses = [], []
    for index, formal in enumerate(fixed):
        if index not in bound:
            if not formal.get("optional", False):
                raise ValueError("missing required argument " + formal["name"])
            slots.append("None")
            statuses.append("implicit_omit")
        else:
            slot, status = bound[index]
            slots.append(slot)
            statuses.append(status)
    if has_rest:
        slots.append(rest_items)
        statuses.append(rest_mode if rest_mode is not None else "rest_empty")
    if any(b <= a for a, b in zip(
            evaluated_call_item_indices, evaluated_call_item_indices[1:])):
        raise AssertionError("evaluated argument indexes must preserve lexical order")
    return {"slots": slots, "statuses": statuses, "evaluation_order": order,
            "call_item_index_by_evaluated_source": evaluated_call_item_indices,
            "omission_bindings": explicit_omission_bindings}


def admit(signatures):
    ranges = []
    for signature in signatures:
        rest = bool(signature and isinstance(signature[-1], dict)
                    and signature[-1].get("rest", False))
        fixed = signature[:-1] if rest else signature
        required = sum(1 for optional in fixed if optional is False)
        ranges.append((required, float("inf") if rest else len(fixed)))
    for i, (lower, upper) in enumerate(ranges):
        for other_lower, other_upper in ranges[i + 1:]:
            if max(lower, other_lower) <= min(upper, other_upper):
                return False
    return True


def main():
    data = json.loads(CASES.read_text(encoding="utf-8"))
    for case in data["accepts"]:
        result = bind(case.get("parameters", data["parameters"]), case["args"])
        for field in ("slots", "statuses", "evaluation_order",
                      "call_item_index_by_evaluated_source", "omission_bindings"):
            if field in case:
                assert result[field] == case[field], (
                    case["id"], field, result[field], case[field])
    for case in data["rejects"]:
        try:
            bind(case.get("parameters", data["parameters"]), case["args"])
        except ValueError as exc:
            assert case["error"] in str(exc), (case["id"], str(exc))
        else:
            raise AssertionError("expected rejection: " + case["id"])
    for case in data["overloads"]:
        assert admit(case["signatures"]) == case["admit"], case["id"]
    print("Reference binder: %s accept, %s reject, %s overload cases PASS" %
          (len(data["accepts"]), len(data["rejects"]), len(data["overloads"])))


if __name__ == "__main__":
    main()
