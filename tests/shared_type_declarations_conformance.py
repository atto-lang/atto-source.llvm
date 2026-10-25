#!/usr/bin/env python3
"""Exact-source shared type-declaration parity matrix (all checks survive -O)."""
import argparse
import csv
from pathlib import Path
import subprocess
import tempfile

CONTRACT = Path(__file__).resolve().parents[1] / "contracts/v1/type-declarations-conformance-v1.tsv"


def vectors():
    result, names = [], set()
    with CONTRACT.open(encoding="utf-8", newline="") as handle:
        for parts in csv.reader(handle, delimiter="\t"):
            if not parts or parts[0].startswith("#"):
                continue
            if len(parts) != 5 or parts[0] in names or parts[1] not in ("+", "-"):
                raise ValueError(f"invalid or duplicate conformance vector {parts!r}")
            names.add(parts[0])
            result.append((*parts[:4], parts[4].replace("\\n", "\n")))
    if len(result) != 22:
        raise AssertionError(("conformance corpus size", len(result)))
    return result


def run(args):
    return subprocess.run(args, capture_output=True, text=True, timeout=45)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    cfg = parser.parse_args()
    pairs = {}
    with tempfile.TemporaryDirectory(prefix="ores-shared-type-") as root:
        source = Path(root) / "shared.ores"  # stable path for byte-identical IR
        ll = Path(root) / "shared.ll"
        binary = Path(root) / "shared"
        for name, outcome, group, diagnostic, program in vectors():
            source.write_text(program, encoding="utf-8")
            result = run([cfg.compiler, str(source)])
            if outcome == "+":
                if result.returncode or result.stderr.strip() or not result.stdout.startswith("; ModuleID"):
                    raise AssertionError((name, "LLVM emission failed", result.returncode, result.stderr, result.stdout[:250]))
                for forbidden in ("@Code(", "@Tagged(", "@Endpoint(", "@Number("):
                    if forbidden in result.stdout:
                        raise AssertionError((name, "type declaration emitted callable", forbidden))
                pairs.setdefault(group, []).append((name, result.stdout))
                if name == "basic-declare":
                    ll.write_text(result.stdout, encoding="utf-8")
                    linked = run([cfg.clang, "-x", "ir", str(ll), "-o", str(binary)])
                    if linked.returncode:
                        raise AssertionError((name, "Clang failed", linked.stderr))
                    executed = run([str(binary)])
                    if executed.returncode != 7:
                        raise AssertionError((name, "native return code != 7", executed.returncode, executed.stderr))
            else:
                if result.returncode == 0 or result.stdout.strip():
                    raise AssertionError((name, "invalid program admitted or emitted IR", result.returncode, result.stdout[:250]))
                if diagnostic != "-" and diagnostic not in result.stderr:
                    raise AssertionError((name, "wrong diagnostic", diagnostic, result.stderr))
    if set(pairs) != {"basic", "public", "module", "kinds", "forward"}:
        raise AssertionError(("missing equivalence groups", sorted(pairs)))
    for group, results in pairs.items():
        if len(results) != 2 or results[0][1] != results[1][1]:
            raise AssertionError((group, "non-identical declare/define IR", [r[0] for r in results]))
    print("PASS: 22 shared cases, five IR-equivalent pairs, Clang-linked native exit 7")


if __name__ == "__main__":
    main()
