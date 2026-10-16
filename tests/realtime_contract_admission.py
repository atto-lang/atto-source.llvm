#!/usr/bin/env python3
"""Fail-closed LLVM realtime contract admission and metadata regression matrix."""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

ACTOR = """define actor Controller as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
"""
VALID = ("@realtime(mode: #soft, period_us: 1000, deadline_us: 900, "
         "budget_us: 100, mailbox: 16)\n")
NEGATIVES = {
    "hard_unproven": VALID.replace("#soft", "#hard"),
    "bounded_unproven": VALID.replace("#soft", "#bounded"),
    "unknown_mode": VALID.replace("#soft", "#fast"),
    "missing_mode": VALID.replace("mode: #soft, ", ""),
    "missing_mailbox": VALID.replace(", mailbox: 16", ""),
    "duplicate_budget": VALID.replace("budget_us: 100", "budget_us: 100, budget_us: 100"),
    "unknown_field": VALID.replace("mailbox: 16", "capacity: 16"),
    "zero_period": VALID.replace("period_us: 1000", "period_us: 0"),
    "zero_budget": VALID.replace("budget_us: 100", "budget_us: 0"),
    "budget_over_deadline": VALID.replace("budget_us: 100", "budget_us: 901"),
    "deadline_over_period": VALID.replace("deadline_us: 900", "deadline_us: 1001"),
    "mailbox_zero": VALID.replace("mailbox: 16", "mailbox: 0"),
    "mailbox_over": VALID.replace("mailbox: 16", "mailbox: 17"),
    "negative_number": VALID.replace("budget_us: 100", "budget_us: -1"),
    "period_over_day": VALID.replace("period_us: 1000", "period_us: 86400000001"),
    "integer_overflow": VALID.replace("period_us: 1000", "period_us: 9223372036854775808"),
    "unknown_attribute": VALID.replace("@realtime", "@realtime_unverified"),
    "stray_attribute": VALID + ACTOR + VALID,
    "trailing_comma": VALID.replace("mailbox: 16", "mailbox: 16,"),
}
EXPECTED_ERRORS = {
    "hard_unproven": "certified WCET",
    "bounded_unproven": "verified allocation",
    "unknown_mode": "unknown @realtime mode",
    "missing_mode": "@realtime requires",
    "duplicate_budget": "duplicate @realtime field",
    "unknown_field": "unknown @realtime field",
    "zero_period": "@realtime requires",
    "zero_budget": "@realtime requires",
    "budget_over_deadline": "@realtime requires",
    "deadline_over_period": "@realtime requires",
    "mailbox_zero": "@realtime requires",
    "mailbox_over": "@realtime requires",
    "period_over_day": "@realtime requires",
    "integer_overflow": "integer literal overflows",
    "unknown_attribute": "unknown native attribute",
}
FORBIDDEN_FILES = {".ll", ".bc", ".o", ".out", ".exe"}


def compile_source(compiler, root, target, label, contents):
    case = pathlib.Path(root) / target / label
    case.mkdir(parents=True)
    source = case / "source.ores"
    source.write_text(contents, encoding="utf-8")
    result = subprocess.run(
        [compiler, "--target=" + target, str(source)],
        cwd=case, capture_output=True, text=True, timeout=35, check=False)
    artifacts = [str(p.relative_to(case))
                 for p in case.rglob("*") if p.is_file() and p != source]
    return result, artifacts


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    args = parser.parse_args()
    found = shutil.which(args.compiler)
    if not found:
        raise RuntimeError("missing native compiler " + args.compiler)
    compiler = str(pathlib.Path(found).resolve())
    failures = []
    with tempfile.TemporaryDirectory(prefix="oreslang-realtime-v1-") as root:
        for target in ("host", "wasm32"):
            valid, files = compile_source(compiler, root, target, "soft", VALID + ACTOR)
            if valid.returncode or files:
                failures.append(target + ": soft contract rejected/wrote files: " +
                                valid.stderr[:320] + " " + repr(files))
            else:
                ir = valid.stdout
                expected = ('!oreslang.realtime.soft.v1', '!"Controller"',
                            '!"1000"', '!"900"', '!"100"', '!"16"')
                if any(fragment not in ir for fragment in expected):
                    failures.append(target + ": declared typed contract missing from IR metadata")
            control, files = compile_source(compiler, root, target, "control", ACTOR)
            if control.returncode or files:
                failures.append(target + ": legacy actor regression: " +
                                control.stderr[:320])
            elif "!oreslang.realtime.soft.v1" in control.stdout:
                failures.append(target + ": unannotated actor gained realtime metadata")
            for label, attribute in NEGATIVES.items():
                source = (attribute if label == "stray_attribute"
                          else attribute + ACTOR)
                result, artifacts = compile_source(compiler, root, target, label, source)
                if result.returncode == 0 or result.stdout.strip() or artifacts:
                    failures.append(target + "/" + label +
                                    ": fail-closed rejection failed; status=" +
                                    str(result.returncode) + " stdout=" +
                                    repr(result.stdout[:100]) + " files=" + repr(artifacts))
                if not result.stderr.strip():
                    failures.append(target + "/" + label + ": missing diagnostic")
                diagnostic = EXPECTED_ERRORS.get(label)
                if diagnostic and diagnostic not in result.stderr:
                    failures.append(target + "/" + label +
                                    ": wrong diagnostic " + repr(result.stderr[:300]))
            invalid_target, artifacts = compile_source(
                compiler, root, target, "function", VALID +
                "pub fnc main(): int { return 42; }\n")
            if invalid_target.returncode == 0 or invalid_target.stdout.strip() or artifacts:
                failures.append(target + ": function attribute was incorrectly admitted")
        # This is compile-time metadata only, not a physical timing test.
        if failures:
            for failure in failures:
                print("FAIL:", failure, file=sys.stderr)
            return 1
    print("PASS: LLVM realtime v1 typed soft metadata; host/wasm fail-closed admission")
    return 0


if __name__ == "__main__":
    sys.exit(main())
