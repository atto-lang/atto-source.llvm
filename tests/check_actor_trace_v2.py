#!/usr/bin/env python3
"""Fail-closed finite trace checker for atto.actor-trace.v2.

Checks the event order *as observed by the producer*. It cannot establish
that native instrumentation is complete or correctly positioned. In
particular, reconstructed-after-quiescence histories are not clock evidence.
No third-party dependencies or cross-repository network access are needed.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import sys

CONTRACT = Path(__file__).resolve().parent.parent / "contracts" / "actor-trace-v2.json"
ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._:@#-]{0,127}$")
HEX = re.compile(r"^[a-f0-9]{64}$")
MAX_BYTES = 8_000_000


class TraceViolation(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise TraceViolation(reason)


def identifier(value, label):
    require(type(value) is str and ID.fullmatch(value) is not None, f"invalid {label}")


def uint(value, label, zero=False):
    require(type(value) is int and (value >= 0 if zero else value > 0)
            and value <= 2**53 - 1, f"invalid {label}")


def unique_pairs(pairs):
    out = {}
    for key, value in pairs:
        require(key not in out, f"duplicate JSON field {key}")
        out[key] = value
    return out


def load_json(path):
    data = Path(path).read_bytes()
    require(len(data) <= MAX_BYTES, "trace byte budget exceeded")
    try:
        return json.loads(data, object_pairs_hook=unique_pairs,
                          parse_constant=lambda s: (_ for _ in ()).throw(
                              TraceViolation("nonfinite JSON number: " + s)))
    except (UnicodeError, json.JSONDecodeError) as exc:
        raise TraceViolation(f"invalid JSON: {exc}") from exc


def verify_trace(receipt, contract=None):
    contract = contract if contract is not None else json.loads(CONTRACT.read_text())
    require(type(receipt) is dict, "receipt must be a JSON object")
    require(set(receipt) == {"schema", "complete", "producer", "events"},
            "receipt keys must exactly match v2 contract")
    require(receipt["schema"] == contract["schema"], "wrong schema")
    require(receipt["complete"] is True, "incomplete trace cannot pass a required gate")
    prod = receipt["producer"]
    require(type(prod) is dict and set(prod) == set(contract["producer"]["required"]),
            "invalid producer fields")
    for key, allowed in (("backend", "backends"), ("subject", "subjects"),
                         ("emission", "emissions")):
        require(type(prod[key]) is str and prod[key] in contract["producer"][allowed],
                f"invalid producer {key}")
    require(not (prod["subject"] == "compiled_atto_guest"
                 and prod["emission"] == "reconstructed_after_quiescence"),
            "post-hoc reconstructed history cannot claim compiled guest instrumentation")
    require(not (prod["subject"] == "reference_model" and prod["backend"] != "reference"),
            "reference model must identify itself as reference")
    events = receipt["events"]
    require(type(events) is list, "events must be an array")
    require(0 < len(events) <= contract["max_events"], "event budget exceeded or vacuous trace")

    actors = {}
    seen_message_ids = set()
    counts = {"spawns": 0, "admitted": 0, "completed": 0,
              "discarded": 0, "control_dispatches": 0,
              "parallel_receives_witnessed": False, "parallel_reads_witnessed": False}

    for i, event in enumerate(events):
        require(type(event) is dict, f"event {i} not an object")
        kind = event.get("kind")
        require(type(kind) is str and kind in contract["events"],
                f"unsupported event kind at {i}")
        require(set(event) == set(contract["event_base_fields"]) | set(contract["events"][kind]),
                f"wrong event fields for {kind} at {i}")
        uint(event["seq"], "seq", zero=True)
        require(event["seq"] == i, f"noncontiguous sequence at {i}")
        identifier(event["actor_id"], "actor_id")
        uint(event["generation"], "generation")
        actor_id, gen = event["actor_id"], event["generation"]
        if kind == "actor_spawn":
            require(type(event["serial"]) is bool, "serial must be boolean")
            last = actors.get(actor_id)
            if last is None:
                require(gen == 1, "first incarnation must be generation 1")
            else:
                require(last["retired"] and gen == last["gen"] + 1,
                        "restart requires retirement and next generation")
            require(counts["spawns"] < contract["max_actor_incarnations"],
                    "actor-incarnation budget exceeded")
            actors[actor_id] = {
                "gen": gen, "serial": event["serial"], "closed": False,
                "fenced": False, "retired": False, "control": False,
                "ready": [], "active": {}, "readers": set(), "writer": None,
            }
            counts["spawns"] += 1
            continue
        a = actors.get(actor_id)
        require(a is not None and not a["retired"], "unknown or retired actor")
        require(gen == a["gen"], "stale actor generation")
        if "message_id" in event:
            identifier(event["message_id"], "message_id")
            msg = event["message_id"]
        if "receiver_id" in event:
            identifier(event["receiver_id"], "receiver_id")
            rid = event["receiver_id"]

        if kind == "send_accept":
            require(not a["closed"] and not a["fenced"], "send after close/fence")
            require(msg not in seen_message_ids, "duplicate/replayed message ID")
            require(len(a["ready"]) < 10000, "bounded mailbox exceeded")
            a["ready"].append(msg)
            seen_message_ids.add(msg)
            counts["admitted"] += 1
        elif kind == "control_admit":
            require(not a["closed"] and not a["fenced"] and not a["control"],
                    "CONTROL admission duplicated or actor unavailable")
            a["control"] = True
        elif kind == "control_dispatch":
            require(a["control"] and not a["fenced"], "CONTROL dispatch without admission")
            a["control"], a["fenced"] = False, True
            counts["control_dispatches"] += 1
        elif kind == "receive_start":
            require(not a["control"] and not a["fenced"], "DATA overtook pending CONTROL/fence")
            require(a["ready"] and a["ready"][0] == msg, "wrong FIFO or unadmitted DATA")
            require(rid not in a["active"], "receiver reentered before completing")
            require(not a["serial"] or not a["active"], "serial actor ran parallel receiver")
            require(len(a["active"]) < contract["max_active_receivers_per_actor"],
                    "active receiver budget exceeded")
            a["ready"].pop(0)
            a["active"][rid] = msg
            if len(a["active"]) > 1:
                counts["parallel_receives_witnessed"] = True
        elif kind == "receive_complete":
            require(a["active"].get(rid) == msg, "completion without matching receiver/ID")
            require(rid not in a["readers"] and a["writer"] != rid,
                    "receiver completed while holding critical lock")
            del a["active"][rid]
            counts["completed"] += 1
        elif kind == "message_discard":
            require(a["closed"] or a["fenced"], "discard before close/fence")
            require(msg in a["ready"], "discard of running/unknown/settled message")
            a["ready"].remove(msg)
            counts["discarded"] += 1
        elif kind == "critical_read_enter":
            require(rid in a["active"] and rid not in a["readers"]
                    and a["writer"] != rid and a["writer"] is None,
                    "invalid concurrent critical-read")
            a["readers"].add(rid)
            if len(a["readers"]) > 1:
                counts["parallel_reads_witnessed"] = True
        elif kind == "critical_read_exit":
            require(rid in a["readers"], "unmatched critical-read exit")
            a["readers"].remove(rid)
        elif kind == "critical_write_enter":
            require(rid in a["active"] and not a["readers"] and a["writer"] is None,
                    "critical-write overlaps another lock or nonrunning receiver")
            a["writer"] = rid
        elif kind == "critical_write_exit":
            require(a["writer"] == rid, "critical-write exited by nonowner")
            a["writer"] = None
        elif kind == "actor_close":
            require(not a["closed"], "actor closed twice")
            a["closed"] = True
        elif kind == "actor_retire":
            require(a["closed"], "actor retired without closing")
            require(not a["control"], "retired with pending CONTROL")
            require(not a["ready"] and not a["active"], "retired with live DATA/receiver")
            require(not a["readers"] and a["writer"] is None,
                    "retired with active critical lock")
            a["retired"] = True

    require(bool(actors) and all(a["retired"] for a in actors.values()),
            "complete trace must retire every actor")
    require(counts["admitted"] == counts["completed"] + counts["discarded"],
            "admission conservation failed")
    return counts


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("trace", nargs="+", type=Path)
    args = ap.parse_args(argv)
    try:
        for trace in args.trace:
            counts = verify_trace(load_json(trace))
            print("ACTOR_TRACE_V2_PASS", trace, json.dumps(counts, sort_keys=True))
    except (TraceViolation, OSError, TypeError, KeyError) as exc:
        print(f"ACTOR_TRACE_V2_REJECTED: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
