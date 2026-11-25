#!/usr/bin/env python3
"""Compiler-backed source field restoration matrix; never a static guest pass.

Every positive must compile actual .atto with oreslang-llvmc, link with real
Clang and return its own expected native exit. Every negative must be rejected
by the semantic ownership checker with its required diagnostic text.
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

HERE = pathlib.Path(__file__).resolve().parent
MANIFEST = HERE / "contracts" / "atto_ownership_source_cfg_50.json"
CONTRACT = HERE / "contract.py"
CATEGORIES = {
    "restored-direct": None,
    "restored-both-branches": None,
    "restored-sibling": None,
    "unrestored-one-branch": "conditional CFG predecessor ownership mismatch",
    "unrestored-read": "ownership violation: use-after-move class field",
}
ID = re.compile(r"^[a-z][a-z0-9-]{1,70}$")


class MatrixFailure(RuntimeError):
    pass


def require(valid: bool, message: str) -> None:
    if not valid:
        raise MatrixFailure(message)


def executable(source: str) -> str:
    # Source-shape checks are a non-authoritative inventory, additionally to
    # actual production compiler and Clang/guest execution.
    cleaned = re.sub(r"/\*.*?\*/", "", source, flags=re.S)
    return "\n".join(line.split("//", 1)[0]
                     for line in cleaned.splitlines())


def load_cases(manifest: pathlib.Path = MANIFEST) -> list[dict]:
    try:
        doc = json.loads(manifest.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise MatrixFailure("missing or malformed source CFG manifest") from error
    require(type(doc) is dict and set(doc) ==
            {"schema_version", "classification", "requirements", "cases"},
            "unexpected manifest envelope")
    require(type(doc["schema_version"]) is int and doc["schema_version"] == 1,
            "unknown source CFG manifest version")
    require(doc["classification"] ==
            "CANDIDATE_REAL_SOURCE_CFG_COMPILER_INPUTS_NOT_EXECUTED",
            "static fixtures must not declare guest execution")
    require(doc["requirements"] == {
        "real_compiler_binary": True,
        "semantic_diagnostic_on_negative": True,
        "clang_link_and_guest_exit_on_positive": True,
        "exact_50_distinct_source_programs": True,
        "path_sensitive_source_restoration_contract": True,
    }, "mandatory source CFG execution requirements missing")
    cases = doc["cases"]
    require(type(cases) is list and len(cases) == 50,
            "exactly 50 compiler-backed CFG programs required")
    ids, digests = set(), set()
    categories = Counter()
    for case in cases:
        require(type(case) is dict, "invalid source CFG row")
        required = {"id", "category", "source"}
        require(set(case) in (required | {"expected_guest_exit"},
                             required | {"expected_diagnostic"}),
                "exactly one diagnostic or guest result is mandatory")
        name, category, source = (case["id"], case["category"],
                                  case["source"])
        require(type(name) is str and ID.fullmatch(name) and
                name not in ids, "invalid/duplicate CFG fixture ID")
        require(type(category) is str and category in CATEGORIES and
                name.startswith(category + "-"),
                "forged category/fixture ID")
        require(type(source) is str and source.strip() and
                "\x00" not in source and len(source) <= 32768,
                "empty, binary or oversized .atto compiler input")
        digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
        require(digest not in digests, "duplicate CFG source input")
        ids.add(name)
        digests.add(digest)
        categories[category] += 1

        code = executable(source)
        require("define class " in code and "pub fnc main(): int" in code and
                "let mut " in code and
                "rt take p.left" in code,
                "source does not exercise compiler-owned partial field move")
        # Correlate the compiler input's class and branch truth with the
        # expected process exit. A valid-looking but wrong exit integer is
        # not a meaningful independent guest oracle.
        index = int(name.rsplit("-", 1)[1]) - 1
        require(0 <= index < 10, "fixture sequence out of bounds")
        base = 20 + index
        declaration = re.search(
            r"define\s+class\s+Restore([0-9]+)\s+as\s+"
            r"pub\s+let\s+int\s+left\s*=\s*([0-9]+);\s+"
            r"pub\s+let\s+int\s+right\s*=\s*7;\s+end\b",
            code,
        )
        require(declaration is not None and
                int(declaration.group(1)) == index and
                int(declaration.group(2)) == base and
                f"new Restore{index}()" in code,
                "class owner/default field oracle cannot be reconstructed")
        branch_category = category in {
            "restored-both-branches", "unrestored-one-branch"
        }
        predicate = re.findall(
            r"\bif\s*\(\s*([0-9]+)\s+eq\s+([0-9]+)\s*\)",
            code,
        )
        if branch_category:
            require(len(predicate) == 1,
                    "missing or duplicated branch predicate")
            left, right = (int(value) for value in predicate[0])
            require(left == base and
                    right == base + (1 if index >= 5 else 0),
                    "branch suite must contain five true and five false paths")
        else:
            require(not predicate and "if (" not in code,
                    "non-branch category contains an unverified branch")
        assignments = re.findall(r"\bp\.left\s*=\s*saved", code)
        if category == "restored-direct":
            require(len(assignments) == 1 and "return p.left + 1" in code and
                    "if (" not in code,
                    "direct restoration source shape missing")
        elif category == "restored-both-branches":
            # The branch-exit oracle assumes the false path performs
            # saved+1. Count-only matching would accept saved+2 and could
            # silently leave a wrong, yet in-range, exit expectation.
            expected_arms = re.search(
                r"\bif\s*\([^)]*\)\s*\{\s*p\.left\s*=\s*saved\s*;\s*"
                r"\}\s*else\s*\{\s*p\.left\s*=\s*saved\s*\+\s*1\s*;\s*"
                r"\}\s*fi\b",
                code,
            )
            require(len(assignments) == 2 and
                    expected_arms is not None and
                    "return p.left;" in code,
                    "both-predecessor restoration shape missing")
        elif category == "restored-sibling":
            require(len(assignments) == 1 and
                    "return p.left + p.right" in code,
                    "independent sibling read missing")
        elif category == "unrestored-one-branch":
            require(len(assignments) == 1 and "else { } fi" in code and
                    "return p.left;" in code,
                    "one-predecessor denial case substituted")
        elif category == "unrestored-read":
            require(not assignments and "return p.left + saved;" in code,
                    "read-after-move denial case substituted")
        diag = CATEGORIES[category]
        if diag:
            require(set(case) == required | {"expected_diagnostic"} and
                    case["expected_diagnostic"] == diag,
                    "negative semantic ownership oracle invalid")
        else:
            expected_exit = (
                base + 1 if category == "restored-direct" else
                base + 7 if category == "restored-sibling" else
                base + (1 if index >= 5 else 0)
            )
            require(set(case) == required | {"expected_guest_exit"} and
                    type(case["expected_guest_exit"]) is int and
                    case["expected_guest_exit"] == expected_exit and
                    1 <= expected_exit <= 255,
                    "positive exit oracle does not match the executed branch")
    require(set(categories) == set(CATEGORIES) and
            all(count == 10 for count in categories.values()),
            "matrix must have exactly ten distinct sources per category")
    return cases


def run_matrix(compiler: pathlib.Path, clang: pathlib.Path) -> None:
    require(compiler.is_file(), "production LLVM compiler is missing")
    require(clang.is_file(), "Clang linker/compiler is missing")
    require(CONTRACT.is_file(), "contract.py compiler harness is missing")
    cases = load_cases()
    executed = 0
    with tempfile.TemporaryDirectory(prefix="atto-source-cfg-50-") as directory:
        root = pathlib.Path(directory)
        for case in cases:
            source = root / (case["id"] + ".atto")
            source.write_text(case["source"], encoding="utf-8")
            args = [sys.executable, str(CONTRACT), "--compiler",
                    str(compiler), "--fixture", str(source)]
            if "expected_diagnostic" in case:
                args += ["--diagnostic", case["expected_diagnostic"]]
            else:
                args += ["--run", str(case["expected_guest_exit"]),
                         "--clang", str(clang)]
            try:
                result = subprocess.run(args, capture_output=True,
                                        text=True, check=False, timeout=120)
            except subprocess.TimeoutExpired as error:
                raise MatrixFailure("compiler/guest timed out: " + case["id"]) from error
            require(result.returncode == 0,
                    "compiler/guest oracle failed for " + case["id"] + ": " +
                    (result.stdout + result.stderr)[-2400:])
            executed += 1
            print("CFG_COMPILER_CASE_PASS " + case["id"], flush=True)
    require(executed == 50, "incomplete source CFG guest matrix")
    print("ATTO_SOURCE_CFG_REAL_COMPILER_FIXTURES=50/50_EXECUTED", flush=True)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    parser.add_argument("--clang", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        run_matrix(args.compiler.resolve(), args.clang.resolve())
    except MatrixFailure as error:
        print("ATTO_SOURCE_CFG_MATRIX_FAILED: " + str(error),
              file=sys.stderr, flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
