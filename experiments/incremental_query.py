"""Experimental, backend-neutral Oreslang incremental query model.

NOT a language parser, compiler, or production cache. This module exercises
red/green dependency tracking, negative lookups, specialization boundaries,
and backend-independent vs backend-specific query identities.
No security-sensitive compiler proof should consume this model as authority.
"""

from __future__ import annotations

from collections import Counter
from copy import deepcopy
from dataclasses import dataclass
from hashlib import sha256
import json
import math
from typing import Any


class QueryCycleError(RuntimeError):
    """An invalid cycle was detected in the declared computation graph."""


@dataclass(frozen=True)
class QueryResult:
    value: Any
    fingerprint: str
    dependencies: tuple[tuple[tuple[str, str], str], ...]


@dataclass(frozen=True)
class _MissingInput:
    """A distinct identity for absence; never collides with user JSON values."""

    name: str


class QueryEngine:
    """Minimal red/green, in-memory reference query engine.

    This demonstrates key semantic invalidation edges, rather than providing a
    parser, stable AST/IR format, or validated borrow checker. Inputs must be
    canonical JSON values. Do not use this as a trust boundary.
    """

    def __init__(self, schema_version: str = "incremental-demo-v1") -> None:
        if not isinstance(schema_version, str) or not schema_version:
            raise ValueError("A nonempty semantic schema version is required")
        self._schema_version = schema_version
        self._inputs: dict[str, Any] = {}
        self._cache: dict[tuple[str, str], QueryResult] = {}
        self.computations: Counter[tuple[str, str]] = Counter()
        self._frames: list[dict[tuple[str, str], str]] = []
        self._active: set[tuple[str, str]] = set()

    @property
    def schema_version(self) -> str:
        return self._schema_version

    @staticmethod
    def _snapshot(value: Any) -> Any:
        """Accept only bounded canonical JSON, and take a defensive snapshot."""
        def walk(v: Any, depth: int = 0) -> Any:
            if depth > 64:
                raise ValueError("Input nesting exceeds 64 levels")
            if v is None or type(v) in (str, int, bool):
                return v
            if type(v) is float:
                if not math.isfinite(v):
                    raise ValueError("Nonfinite numbers are not valid inputs")
                return v
            if type(v) is list:
                return [walk(item, depth + 1) for item in v]
            if type(v) is dict:
                if not all(type(key) is str for key in v):
                    raise TypeError("Every JSON object key must be a string")
                return {key: walk(item, depth + 1) for key, item in v.items()}
            raise TypeError("Inputs must be canonical JSON values")

        snapshot = walk(value)
        data = json.dumps(
            snapshot, sort_keys=True, ensure_ascii=False,
            separators=(",", ":"), allow_nan=False,
        ).encode("utf-8")
        if len(data) > 1_048_576:
            raise ValueError("Input exceeds the 1 MiB reference-model limit")
        return snapshot

    def _digest(self, value: Any) -> str:
        # Tagged envelopes make missing facts impossible to forge with JSON.
        payload = (
            {"kind": "missing-input", "name": value.name}
            if isinstance(value, _MissingInput)
            else {"kind": "present-value", "value": value}
        )
        encoded = json.dumps(
            {"schema": self._schema_version, "payload": payload},
            ensure_ascii=False,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
        return sha256(encoded).hexdigest()

    def set_input(self, name: str, value: Any) -> bool:
        """Update a source fact. Return True only for meaningful changes."""
        if not isinstance(name, str) or not name:
            raise ValueError("Input names must be nonempty strings")
        snapshot = self._snapshot(value)
        previous = self._inputs.get(name, _ABSENT)
        if previous is not _ABSENT and self._digest(previous) == self._digest(snapshot):
            return False
        self._inputs[name] = snapshot
        self._cache.pop(("input", name), None)
        return True

    def result(self, kind: str, symbol: str) -> QueryResult:
        """Evaluate a query and return the value plus its semantic fingerprint."""
        if not isinstance(kind, str) or not isinstance(symbol, str):
            raise TypeError("Query kind and identity must be strings")
        query = self._evaluate((kind, symbol))
        return QueryResult(deepcopy(query.value), query.fingerprint, query.dependencies)

    def use(self, kind: str, symbol: str) -> Any:
        """Record a direct dependency while computing a parent query."""
        key = (kind, symbol)
        child = self._evaluate(key)
        if self._frames:
            self._frames[-1][key] = child.fingerprint
        return deepcopy(child.value)

    def _evaluate(self, key: tuple[str, str]) -> QueryResult:
        if key in self._active:
            raise QueryCycleError("Cyclic query: " + repr(key))
        if len(self._active) >= 64:
            raise QueryCycleError("Query depth exceeds the reference-model limit")
        self._active.add(key)
        try:
            cached = self._cache.get(key)
            if cached is not None:
                # Validation uses _evaluate, not use: we must not accidentally
                # record transitive dependencies in the caller's query frame.
                green = True
                for dep, fingerprint in cached.dependencies:
                    if self._evaluate(dep).fingerprint != fingerprint:
                        green = False
                        break
                if green:
                    return cached

            self._frames.append({})
            try:
                value = self._compute(*key)
                deps = tuple(sorted(self._frames[-1].items()))
                result = QueryResult(value, self._digest(value), deps)
            finally:
                self._frames.pop()
            self.computations[key] += 1
            self._cache[key] = result
            return result
        finally:
            self._active.remove(key)

    def _compute(self, kind: str, identity: str) -> Any:
        if kind == "input":
            if identity not in self._inputs:
                return _MissingInput(identity)
            return self._inputs[identity]
        if kind == "lookup":
            names = self.use("input", "registry")
            if not isinstance(names, list) or not all(isinstance(n, str) for n in names):
                raise ValueError("Registry must be a list of symbol names")
            if len(names) != len(set(names)):
                raise ValueError("Duplicate symbols in registry")
            return identity in names
        if kind == "interface":
            if not self.use("lookup", identity):
                return {"missing": identity}
            signature = self.use("input", "signature:" + identity)
            effects = self.use("input", "effects:" + identity)
            if isinstance(signature, _MissingInput) or isinstance(effects, _MissingInput):
                return {"missing": identity}
            return {"signature": signature, "effects": effects}
        if kind == "typed":
            own_interface = self.use("interface", identity)
            body = self.use("input", "body:" + identity)
            calls = self.use("input", "calls:" + identity)
            if isinstance(body, _MissingInput) or isinstance(calls, _MissingInput):
                return {"valid": False, "missing_input": identity}
            if not isinstance(calls, list) or not all(isinstance(n, str) for n in calls):
                raise ValueError("Call list must be a list of symbols")
            dependencies = [(name, self.use("interface", name)) for name in calls]
            valid = (
                "missing" not in own_interface
                and all("missing" not in iface for _, iface in dependencies)
            )
            return {
                "valid": valid,
                "body": body,
                "own_interface": own_interface,
                "callee_interfaces": dependencies,
            }
        if kind == "mir":
            typed = self.use("typed", identity)
            return {"verified": typed["valid"], "typed": typed}
        if kind == "object":
            # Backend and function are split with a NUL (for prototype only).
            target, _, function = identity.partition("\0")
            if not target or not function or "\0" in function:
                raise ValueError("Object queries require a single target/function separator")
            mir = self.use("mir", function)
            if not mir["verified"]:
                raise ValueError("Unresolved or invalid function " + function)
            inlines = self.use("input", "inlines:" + function)
            if not isinstance(inlines, list) or not all(isinstance(n, str) for n in inlines):
                raise ValueError("Inlining dependencies must be a list of symbols")
            embedded = [(name, self.use("mir", name)) for name in inlines]
            if any(not inline["verified"] for _, inline in embedded):
                raise ValueError("Invalid inline dependency")
            toolchain = self.use("input", "toolchain:" + target)
            if isinstance(toolchain, _MissingInput):
                raise ValueError("Missing toolchain contract for " + target)
            return {
                "target": target,
                "toolchain": toolchain,
                "mir": mir,
                "inlines": embedded,
            }
        if kind == "link":
            symbols = self.use("input", "registry")
            if not isinstance(symbols, list):
                raise ValueError("Registry must be a list")
            artifacts = [
                (name, self.use("object", identity + "\0" + name))
                for name in sorted(symbols)
            ]
            return {"target": identity, "artifacts": artifacts}
        raise ValueError("Unknown query type: " + kind)


_ABSENT = object()
