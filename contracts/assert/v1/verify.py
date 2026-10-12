#!/usr/bin/env python3
"""Validate assertion fixtures, TypeSpec/JSON Schema parity, and scalar oracle.

This does not claim LLVM guest execution: runtime evidence must be supplied by
separate backend-specific conformance runners.
"""
import argparse
import copy
import json
import re
from pathlib import Path

from jsonschema import Draft202012Validator


TWO = {"equal", "not_equal", "same", "equal_throws", "not_equal_throws", "same_throws"}
ONE = {"true", "false", "true_throws", "false_throws", "fail"}


def scalar(operand):
    kind, value = operand["kind"], operand["value"]
    if kind == "int":
        if re.fullmatch(r"-?(?:0|[1-9][0-9]*)", value) is None:
            raise ValueError("noncanonical integer scalar")
        parsed = int(value)
        if parsed < -(1 << 63) or parsed >= 1 << 63:
            raise ValueError("Oreslang int outside signed 64-bit range")
        return kind, parsed
    if kind == "bool":
        if value not in ("true", "false"):
            raise ValueError("noncanonical boolean scalar")
        return kind, value == "true"
    if kind == "string":
        return kind, value
    raise ValueError("unknown operand kind")


def oracle(case):
    operation = case["operation"]
    operands = [scalar(item) for item in case["operands"]]
    if operation in TWO:
        if len(operands) != 2:
            raise ValueError("binary assertion requires two operands")
        lhs, rhs = operands
        equals = lhs == rhs
        if operation.startswith("not_equal"):
            verdict = not equals
            message = "Assert.not_equal: values are equal"
        elif operation.startswith("same"):
            # Scalar-only fixture subset: primitive identity is value identity.
            verdict = equals
            message = "Assert.same: values do not have the same identity"
        else:
            verdict = equals
            message = "Assert.equal: values are not equal"
    elif operation in ONE:
        if len(operands) != 1:
            raise ValueError("unary assertion requires one operand")
        kind, value = operands[0]
        if operation == "fail":
            if kind != "string":
                raise ValueError("fail requires String")
            verdict = False
            message = value
        else:
            if kind != "bool":
                raise ValueError("boolean assertion requires bool")
            verdict = value if operation.startswith("true") else not value
            message = ("Assert.true: expected true" if operation.startswith("true")
                       else "Assert.false: expected false")
    else:
        raise ValueError("unknown operation")
    failing = operation.endswith("_throws") or operation == "fail"
    if failing and not verdict:
        return ({"kind": "none"} if case["trapped"]
                else {"kind": "throw", "message_contains": message})
    return {"kind": "returned", "boolean": bool(verdict)}


def checks(cases):
    ids = set()
    for case in cases:
        if case["id"] in ids:
            raise ValueError("duplicate case id")
        ids.add(case["id"])
        expected = case["expected"]
        if expected["kind"] == "returned":
            if set(expected) != {"kind", "boolean"}:
                raise ValueError("returned cases require exactly bool outcome")
        elif expected["kind"] == "throw":
            if case["trapped"] or set(expected) != {"kind", "message_contains"}:
                raise ValueError("trapped case may not report ordinary throw")
        elif expected["kind"] == "none":
            if not case["trapped"] or set(expected) != {"kind"}:
                raise ValueError("untrapped case may not report None")
        if oracle(case) != expected:
            raise ValueError("golden mismatch for " + case["id"])


def mutations(suite):
    changes = [
        lambda x: x.pop("schema_version"),
        lambda x: x.update(schema_version="2.0.0"),
        lambda x: x["cases"][0].pop("id"),
        lambda x: x["cases"][0].update(unknown=True),
        lambda x: x["cases"][0].update(operation="unknown"),
        lambda x: x["cases"][0].update(trapped="false"),
        lambda x: x["cases"][0]["operands"][0].update(kind="float"),
        lambda x: x["cases"][0]["operands"][0].update(value=123),
        lambda x: x["cases"][0]["expected"].update(boolean="true"),
        lambda x: x["cases"][0].update(operands="not-an-array"),
    ]
    for change in changes:
        altered = copy.deepcopy(suite)
        change(altered)
        yield altered


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--schema", type=Path, required=True)
    parser.add_argument("--suite", type=Path, required=True)
    parser.add_argument("--typespec-schema", type=Path)
    args = parser.parse_args()
    normative = json.loads(args.schema.read_text())
    suite = json.loads(args.suite.read_text())
    Draft202012Validator.check_schema(normative)
    normative_validator = Draft202012Validator(normative)
    normative_validator.validate(suite)
    checks(suite["cases"])
    validators = [normative_validator]
    if args.typespec_schema is not None:
        generated = json.loads(args.typespec_schema.read_text())
        Draft202012Validator.check_schema(generated)
        validators.append(Draft202012Validator(generated))
        validators[-1].validate(suite)
    for index, altered in enumerate(mutations(suite)):
        results = [not list(validator.iter_errors(altered)) for validator in validators]
        if any(results):
            raise ValueError(f"schema admitted forbidden mutation {index}: {results}")
    label = ("ASSERT_SCHEMA_TYPESPEC_PARITY_PASS" if len(validators) == 2
             else "ASSERT_JSON_SCHEMA_ORACLE_PASS")
    print(f"{label} cases={len(suite['cases'])} "
          f"mutants=10 schemas={len(validators)}")


if __name__ == "__main__":
    main()
