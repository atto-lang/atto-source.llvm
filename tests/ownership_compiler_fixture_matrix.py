#!/usr/bin/env python3
"""Real compiler and Clang/native guest matrix for 50 unique AttoLang sources.

Uses existing tests/contract.py for every row. This runner never synthesizes
a green result: a missing compiler, missing Clang, wrong semantic diagnostic,
fake/textual IR, failed link, wrong guest exit, skipped case, or timeout fails.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import subprocess
import sys
import tempfile
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent
MANIFEST = ROOT / "contracts" / "atto_ownership_place_mir_50.json"
CONTRACT = ROOT / "contract.py"
CATEGORIES = {
    "computed-take": "E_TAKE_UNTRACKED_PLACE",
    "borrowed-scalar-share": "E_SHARE_BORROW",
    "borrowed-field-share": "E_SHARE_BORROW",
    "owned-scalar-share": None,
    "owned-scalar-copy": None,
}
NAME = re.compile(r"^[a-z][a-z0-9-]{1,70}$")
FIELDS = {"id", "category", "source", "expected_diagnostic", "expected_guest_exit"}


class MatrixFailure(Exception):
    pass


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise MatrixFailure(reason)


def load_cases(path: pathlib.Path = MANIFEST) -> list[dict]:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise MatrixFailure("missing or invalid fixture manifest") from exc
    require(isinstance(document, dict) and set(document) ==
            {"schema_version", "classification", "requirements", "cases"},
            "unexpected fixture manifest envelope")
    require(type(document["schema_version"]) is int and document["schema_version"] == 1,
            "unsupported fixture schema version")
    require(document["classification"] == "CANDIDATE_REAL_COMPILER_INPUTS_NOT_EXECUTED",
            "fixture manifest cannot forge an execution verdict")
    requirements = document["requirements"]
    require(isinstance(requirements, dict) and
            set(requirements) == {
                "real_compiler_binary",
                "semantic_diagnostic_on_negative",
                "clang_link_and_guest_exit_on_positive",
                "exact_50_distinct_source_programs"
            } and all(value is True for value in requirements.values()),
            "compiler/guest requirements must all remain enabled")
    cases = document["cases"]
    require(isinstance(cases, list) and len(cases) == 50,
            "exactly fifty compiler fixtures are mandatory")
    seen_names: set[str] = set()
    seen_sources: set[str] = set()
    by_category: Counter = Counter()
    for row in cases:
        require(isinstance(row, dict) and
                set(row) in (FIELDS - {"expected_diagnostic"},
                             FIELDS - {"expected_guest_exit"}),
                "each fixture must request exactly one positive or negative oracle")
        name, category, source = row["id"], row["category"], row["source"]
        require(isinstance(name, str) and NAME.fullmatch(name) and
                name not in seen_names, "invalid or duplicate fixture ID")
        require(isinstance(category, str) and category in CATEGORIES,
                "unknown fixture category")
        require(isinstance(source, str) and source.strip() and
                "\x00" not in source and len(source) < 32768,
                "empty, binary, or oversized .atto source")
        digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
        require(digest not in seen_sources, "duplicate source cannot count twice")
        seen_sources.add(digest)
        seen_names.add(name)
        by_category[category] += 1
        required_diagnostic = CATEGORIES[category]
        if required_diagnostic:
            require(set(row) == FIELDS - {"expected_guest_exit"} and
                    row["expected_diagnostic"] == required_diagnostic,
                    "invalid or substituted semantic ownership diagnostic")
            require("rt take" in source if category == "computed-take" else
                    "rt share" in source,
                    "negative does not exercise the claimed ownership operation")
        else:
            require(set(row) == FIELDS - {"expected_diagnostic"} and
                    type(row["expected_guest_exit"]) is int and
                    1 <= row["expected_guest_exit"] <= 255,
                    "positive case lacks an executable guest exit oracle")
            require(("rt share" if category == "owned-scalar-share" else "rt copy")
                    in source, "positive does not exercise the claimed ownership operation")
    require(set(by_category) == set(CATEGORIES) and
            all(by_category[group] == 10 for group in CATEGORIES),
            "each of the five compiler categories must contain exactly ten cases")
    return cases


def run_matrix(compiler: pathlib.Path, clang: pathlib.Path) -> None:
    require(compiler.is_file(), "production compiler binary does not exist")
    require(clang.is_file(), "native Clang binary does not exist")
    require(CONTRACT.is_file(), "production compiler contract harness unavailable")
    cases = load_cases()
    executed = 0
    with tempfile.TemporaryDirectory(prefix="atto-typed-place-mir-50-") as tmp:
        root = pathlib.Path(tmp)
        for item in cases:
            fixture = root / (item["id"] + ".atto")
            fixture.write_text(item["source"], encoding="utf-8")
            command = [
                sys.executable, str(CONTRACT), "--compiler", str(compiler),
                "--fixture", str(fixture),
            ]
            if "expected_diagnostic" in item:
                command += ["--diagnostic", item["expected_diagnostic"]]
            else:
                command += ["--run", str(item["expected_guest_exit"]),
                            "--clang", str(clang)]
            try:
                result = subprocess.run(command, capture_output=True, text=True,
                                        check=False, timeout=100)
            except subprocess.TimeoutExpired as exc:
                raise MatrixFailure("compiler fixture timed out: " + item["id"]) from exc
            require(result.returncode == 0,
                    "compiler or guest failed for " + item["id"] +
                    ": " + (result.stdout + result.stderr)[-2400:])
            executed += 1
            print("PASS " + item["id"], flush=True)
    require(executed == 50, "a compiler fixture was silently skipped")
    print("ATTO_OWNERSHIP_REAL_COMPILER_FIXTURES=50/50_EXECUTED", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=pathlib.Path)
    parser.add_argument("--clang", required=True, type=pathlib.Path)
    options = parser.parse_args()
    try:
        run_matrix(options.compiler.resolve(), options.clang.resolve())
    except MatrixFailure as exc:
        print("ATTO_OWNERSHIP_FIXTURE_MATRIX_FAILED: " + str(exc),
              file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
