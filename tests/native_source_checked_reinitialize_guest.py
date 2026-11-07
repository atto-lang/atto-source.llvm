#!/usr/bin/env python3
"""Production AttoLang -> LLVM StoreInst provenance -> Clang guest gate.

This checks one bounded straight-line moved-field restoration, not full
typed-MIR CFG, optimization ownership semantics, or LoanId completeness.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parent / "fixtures"
POSITIVE = ROOT / "atto-reinitialize-checked-store-positive.atto"
CONTROL = ROOT / "atto-reinitialize-ordinary-write-control.atto"
NEGATIVE = ROOT / "atto-reinitialize-use-after-move-reject.atto"
REINIT_TAG = b"!atto.ownership.reinitialize.source.v1"


def run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                          check=False, timeout=60)


def require(ok, reason):
    if not ok:
        raise RuntimeError(reason)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=Path)
    parser.add_argument("--clang", required=True, type=Path)
    options = parser.parse_args()
    require(options.compiler.is_file() and options.clang.is_file(),
            "real compiler and clang tools required")
    for path in (POSITIVE, CONTROL, NEGATIVE):
        require(path.is_file(), "missing AttoLang source fixture: " + path.name)

    with tempfile.TemporaryDirectory(prefix="atto-reinit-guest-") as scratch:
        directory = Path(scratch)
        for tag, path, has_reinit in (
            ("checked_reinit", POSITIVE, True),
            ("ordinary_write", CONTROL, False),
        ):
            produced = run([str(options.compiler), str(path)])
            require(produced.returncode == 0 and produced.stdout,
                    f"{tag} real compiler failed: {produced.stderr[:1000]!r}")
            tagged = REINIT_TAG in produced.stdout
            require(tagged == has_reinit,
                    f"{tag} physical LLVM StoreInst provenance tag mismatch")
            ll = directory / f"{tag}.ll"
            exe = directory / tag
            ll.write_bytes(produced.stdout)
            linked = run([str(options.clang), "-O2", "-x", "ir",
                          str(ll), "-o", str(exe)])
            require(linked.returncode == 0,
                    f"{tag} LLVM/Clang link failed: {linked.stderr[:1000]!r}")
            observed = run([str(exe)])
            require(observed.returncode == 42 and
                    observed.stdout == b"" and observed.stderr == b"",
                    f"{tag} native executable behavior mismatch")

        invalid = run([str(options.compiler), str(NEGATIVE)])
        require(invalid.returncode != 0 and invalid.stdout == b"" and
                (b"ownership violation" in invalid.stderr or
                 b"use-after-move" in invalid.stderr or
                 b"moved class field" in invalid.stderr),
                "unrestored moved field source not rejected by real compiler")
    print("ATTO_LLVM_SOURCE_CHECKED_REINITIALIZE=2_GUESTS_1_REJECTION_EXECUTED")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print("SOURCE CHECKED REINITIALIZE GUEST FAILED: " + str(error),
              file=sys.stderr)
        raise SystemExit(1)
