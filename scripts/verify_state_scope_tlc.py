#!/usr/bin/env python3
"""Run the bounded TLA+ proof, unsafe mutants, and reachability witnesses."""
import argparse
import hashlib
import json
import pathlib
import re
import subprocess

TLC_SHA256 = "936a262061c914694dfd669a543be24573c45d5aa0ff20a8b96b23d01e050e88"
INVARIANTS = ("TypeOK SinglePhysicalCarrier CarrierAgreement GuestEntryValid LoanSound "
              "ReadDoesNotWrite RetiredQuiescent ExactlyOnceCleanup")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--jar", required=True)
    parser.add_argument("--java", default="java")
    args = parser.parse_args()
    root, out, jar = map(lambda path: pathlib.Path(path).absolute(), (args.root, args.output, args.jar))
    out.mkdir(parents=True, exist_ok=True)
    (out / "summary.json").unlink(missing_ok=True)
    digest = hashlib.sha256(jar.read_bytes()).hexdigest()
    if digest != TLC_SHA256:
        raise AssertionError("TLC jar differs from the pinned official 1.7.4 release")
    model = root / "spec/formal/state-scopes/StateScopes.tla"
    results = []

    def check(name, mutant="None", expected=None, cover=None):
        directory = out / name
        directory.mkdir(exist_ok=True)
        config = directory / "model.cfg"
        config.write_text('SPECIFICATION FairSpec\nCONSTANT Mutant = "' + mutant + '"\n'
                          + 'INVARIANTS ' + ("TypeOK " + cover if cover else INVARIANTS) + '\n'
                          + ('PROPERTY CancelLeadsToCleanup\n' if not cover else '')
                          + 'CHECK_DEADLOCK FALSE\n')
        command = [args.java, "-Xmx1g", "-XX:+UseParallelGC", "-cp", str(jar), "tlc2.TLC", "-workers", "1", "-seed", "1", "-fp", "0",
                   "-metadir", str(directory / "states"), "-config", str(config), str(model.relative_to(root))]
        run = subprocess.run(command, capture_output=True, text=True, timeout=90, cwd=root)
        text = run.stdout + run.stderr
        (directory / "tlc.log").write_text(text)
        wanted = cover or expected
        if wanted:
            needle = ("Temporal properties were violated" if wanted == "CancelLeadsToCleanup"
                      else "Invariant " + wanted + " is violated")
            expected_code = 13 if wanted == "CancelLeadsToCleanup" else 12
            if run.returncode != expected_code or needle not in text:
                raise AssertionError((name, run.returncode, text))
        elif run.returncode or "Model checking completed. No error has been found." not in text:
            raise AssertionError((name, run.returncode, text))
        counts = re.findall(r"([\d,]+) states generated, ([\d,]+) distinct states found", text)
        if not counts:
            raise AssertionError("missing completed state counts: " + name)
        generated, distinct = [int(value.replace(",", "")) for value in counts[-1]]
        record = {"name": name, "mutant": mutant, "exit_code": run.returncode,
                  "generated": generated, "distinct": distinct,
                  "expected_counterexample": wanted, "passed": True, "command": command}
        results.append(record)
        print("PASS", json.dumps(record), flush=True)

    check("production")
    for mutant, invariant in {
        "Entry": "GuestEntryValid", "ReadWrite": "ReadDoesNotWrite",
        "Yield": "LoanSound", "ForeignRelease": "LoanSound",
        "Retire": "RetiredQuiescent", "DoubleRelease": "ExactlyOnceCleanup",
        "Nested": "LoanSound", "DoubleClaim": "SinglePhysicalCarrier",
        "EarlyLeave": "LoanSound", "Leak": "CancelLeadsToCleanup",
    }.items():
        check("mutant_" + mutant, mutant=mutant, expected=invariant)
    for invariant in ("NeverReadLoan", "NeverWriteLoan", "NeverCancelledLoan",
                      "NeverRecreated", "NeverInterleaved", "NeverCleaned", "NeverGuestWrite"):
        check("cover_" + invariant, cover=invariant)
    summary = {"tlc_release": "1.7.4", "tlc_sha256": digest,
               "model_sha256": hashlib.sha256(model.read_bytes()).hexdigest(),
               "bounds": {"actors": 1, "jobs": 2, "workers": 2, "incarnations": 2,
                          "epochs_per_incarnation": 2, "loan_nonces_per_incarnation": 2,
                          "state_values": 2},
               "fairness": "weak fairness of each job's Unwind, Release and Leave actions",
               "results": results}
    (out / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
