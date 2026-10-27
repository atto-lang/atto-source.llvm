#!/usr/bin/env python3
"""Fail closed if sealed actor state admission ignores alternate lowering metadata."""
from pathlib import Path

path = Path("src/main/cpp/oreslang/backend/SealedStateAdmission.hpp")
text = path.read_text(encoding="utf-8")

required = (
    "field.task_await_name.empty()",
    "field.future_await_name.empty()",
    "!field.assignable",
)
missing = [item for item in required if item not in text]
if missing:
    raise SystemExit(
        "SEALED_STATE_ALTERNATE_LOWERING_GUARD_FAIL missing=" + ",".join(missing)
    )

print("SEALED_STATE_ALTERNATE_LOWERING_GUARD_PASS")
