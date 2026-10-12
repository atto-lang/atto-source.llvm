#!/usr/bin/env python3
"""Validate the normative JSON wire contract and cross-field constraints.

This is a schema conformance fixture, not a privilege check. Runtime policy
MUST verify live scheduler DONE and independently committed external effects.
"""
import copy
import json
from pathlib import Path
from jsonschema import Draft202012Validator, ValidationError

SCHEMA = Path(__file__).resolve().parents[1] / "contracts/v1/supervision/terminal-receipt.schema.json"
schema = json.loads(SCHEMA.read_text())
Draft202012Validator.check_schema(schema)
validator = Draft202012Validator(schema)
UINT64_MAX = 2**64 - 1

def semantic_check(doc):
    validator.validate(doc)
    key = doc["key"]
    actor = key["failed_child"]
    parent = key["original_parent"]
    if key["runtime_id"] != actor["scheduler_domain"] or (
        actor["scheduler_domain"] != parent["scheduler_domain"]
    ):
        raise ValueError("foreign runtime identity")
    for a in (actor, parent):
        if int(a["generation"]) == 0 or int(a["scheduler_domain"]) == 0:
            raise ValueError("zero actor generation/domain")
    if int(key["logical_child_id"]) == 0 or int(key["logical_incarnation"]) == 0:
        raise ValueError("invalid logical actor generation")
    def walk(value):
        if isinstance(value, dict):
            for k, v in value.items():
                if k in ("generation", "supervisor_id", "runtime_id", "scheduler_domain",
                         "actor_slot", "logical_incarnation", "logical_child_id",
                         "failure_code", "correlation", "claim_nonce"):
                    if int(v) > UINT64_MAX:
                        raise ValueError("uint64 overflow")
                walk(v)
        if isinstance(value, list):
            for v in value:
                walk(v)
    walk(doc)
    if "physical_done_proof" in doc:
        if doc["physical_done_proof"]["actor"] != actor:
            raise ValueError("physical DONE evidence belongs to different generation")
    if "external_effect_proof" in doc:
        if doc["external_effect_proof"]["claim_nonce"] != doc["claim_nonce"]:
            raise ValueError("external effect belongs to different claim")
    if "claim_nonce" in doc and int(doc["claim_nonce"]) == 0:
        raise ValueError("missing claim nonce")

base = {
    "schema_version": "1.0.0",
    "key": {
        "supervisor_id": "88",
        "runtime_id": "901",
        "original_parent": {
            "scheduler_domain": "901", "actor_slot": "0", "generation": "1"
        },
        "failed_child": {
            "scheduler_domain": "901", "actor_slot": "1", "generation": "5"
        },
        "failure_kind": "1", "failure_code": "440", "correlation": "750",
        "logical_child_id": "71", "logical_incarnation": "2"
    },
    "phase": "recorded"
}
physical = {
    "actor": copy.deepcopy(base["key"]["failed_child"]),
    "observation_source": "native_scheduler"
}
external = {
    "claim_nonce": "1", "evidence_sha256": "ab" * 32,
    "authority": "host.effect-ledger"
}
phases = ["recorded", "physical_done", "claimed", "effect_proven", "acknowledged"]
for phase in phases:
    doc = copy.deepcopy(base)
    doc["phase"] = phase
    if phase != "recorded":
        doc["physical_done_proof"] = copy.deepcopy(physical)
    if phase in ("claimed", "effect_proven", "acknowledged"):
        doc["claim_nonce"] = "1"
    if phase in ("effect_proven", "acknowledged"):
        doc["external_effect_proof"] = copy.deepcopy(external)
    semantic_check(doc)

# The native journal's uint64 key admits the largest unsigned value
# but never a zero supervisor or zero logical/scheduler generation.
maximum = copy.deepcopy(base)
maximum["key"]["supervisor_id"] = str(UINT64_MAX)
semantic_check(maximum)

invalid = []
wrong = copy.deepcopy(base); wrong["key"]["supervisor_id"] = "0"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["logical_child_id"] = "0"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["original_parent"]["generation"] = "0"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["failed_child"]["generation"] = "0"; invalid.append(wrong)
wrong = copy.deepcopy(base)
wrong["phase"] = "claimed"; wrong["physical_done_proof"] = copy.deepcopy(physical)
wrong["claim_nonce"] = "0"; invalid.append(wrong)
wrong = copy.deepcopy(base)
wrong["phase"] = "effect_proven"; wrong["physical_done_proof"] = copy.deepcopy(physical)
wrong["claim_nonce"] = "1"; wrong["external_effect_proof"] = copy.deepcopy(external)
wrong["external_effect_proof"]["authority"] = "Untrusted Ledger"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["claim_nonce"] = "1"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["failed_child"]["generation"] = 5; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["runtime_id"] = "902"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["key"]["failure_code"] = str(2**64); invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["phase"] = "acknowledged"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["phase"] = "claimed"; wrong["claim_nonce"] = "1"; invalid.append(wrong)
wrong = copy.deepcopy(base); wrong["unexpected"] = True; invalid.append(wrong)
wrong = copy.deepcopy(base)
wrong["phase"] = "physical_done"; wrong["physical_done_proof"] = copy.deepcopy(physical)
wrong["physical_done_proof"]["actor"]["generation"] = "8"; invalid.append(wrong)
wrong = copy.deepcopy(base)
wrong["phase"] = "effect_proven"
wrong["physical_done_proof"] = copy.deepcopy(physical)
wrong["claim_nonce"] = "1"
wrong["external_effect_proof"] = copy.deepcopy(external)
wrong["external_effect_proof"]["claim_nonce"] = "2"
invalid.append(wrong)
for index, doc in enumerate(invalid):
    try:
        semantic_check(doc)
    except (ValidationError, ValueError, TypeError, KeyError):
        continue
    raise AssertionError(f"invalid conformance fixture accepted: {index}")

print("NATIVE_TERMINAL_RECEIPT_SCHEMA_PASS valid=6 invalid=15")
