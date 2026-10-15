#!/usr/bin/env python3
"""Read-only Oreslang formatter corpus audit.

This is deliberately NOT a whole-repository formatting gate. Many fixtures are
intentionally malformed, and the LLVM frontend does not yet support Java-style
imports. This script must never rewrite or replace an input .ores file.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

EXCLUDE = {".git", ".github", "target", "build", "out", "node_modules", ".venv", "__pycache__"}


def source_files(roots: list[Path]) -> list[Path]:
    found: set[Path] = set()
    for root in roots:
        if root.is_symlink():
            continue
        if root.is_file():
            if root.suffix == ".ores":
                found.add(root.resolve())
            continue
        if not root.is_dir():
            raise ValueError(f"input does not exist: {root}")
        for folder, dirs, filenames in os.walk(root, followlinks=False):
            dirs[:] = sorted(
                d for d in dirs
                if d not in EXCLUDE and not (Path(folder) / d).is_symlink()
            )
            for name in filenames:
                path = Path(folder) / name
                if path.suffix == ".ores" and path.is_file() and not path.is_symlink():
                    found.add(path.resolve())
    return sorted(found)


def audit(files: list[Path], binary: str, timeout: int) -> dict:
    changed: list[str] = []
    rejected: list[dict[str, str]] = []
    checked = 0
    for path in files:
        original = path.read_bytes()
        try:
            run = subprocess.run(
                [binary, "--stdout", str(path)],
                capture_output=True,
                check=False,
                timeout=timeout,
            )
        except (OSError, subprocess.TimeoutExpired) as exc:
            rejected.append({"path": str(path), "error": str(exc)[:320]})
            continue
        if run.returncode != 0:
            rejected.append({
                "path": str(path),
                "error": run.stderr.decode("utf-8", errors="replace")[:320],
            })
            continue
        checked += 1
        if run.stdout != original:
            changed.append(str(path))
        # Intentional: never write back, even when the formatter output differs.
    return {
        "files": len(files),
        "formatted_successfully": checked,
        "would_change": len(changed),
        "format_errors": len(rejected),
        "changed_files": changed,
        "rejected_files": rejected,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("roots", nargs="*", type=Path, help="source files or directories")
    parser.add_argument("--formatter", default="oresfmt", help="installed formatter executable")
    parser.add_argument("--timeout", type=int, default=15, help="seconds per file")
    parser.add_argument("--strict", action="store_true", help="fail on any drift or formatter error")
    parser.add_argument("--json", type=Path, help="optional JSON audit output file")
    args = parser.parse_args(argv)
    if args.timeout < 1 or args.timeout > 300:
        parser.error("--timeout must be between 1 and 300 seconds")
    if shutil.which(args.formatter) is None:
        parser.error(f"formatter is not installed or not executable: {args.formatter}")
    try:
        roots = args.roots or [Path(__file__).resolve().parents[1]]
        files = source_files(roots)
        report = audit(files, args.formatter, args.timeout)
    except (OSError, ValueError) as exc:
        parser.error(str(exc))
    summary = (
        f"oresfmt corpus: {report['files']} files, "
        f"{report['formatted_successfully']} formatted, "
        f"{report['would_change']} would change, "
        f"{report['format_errors']} rejected (often intentionally invalid fixtures)"
    )
    print(summary)
    for path in report["changed_files"][:12]:
        print(f"  WOULD-CHANGE {path}")
    for error in report["rejected_files"][:12]:
        print(f"  REJECTED {error['path']}: {error['error']}")
    if args.json:
        # Never truncate an existing file. In particular, --json must not
        # overwrite a .ores source via a direct path, symlink, or hardlink.
        # Exclusive creation also protects against races between checking
        # for existence and opening the report destination.
        if args.json.suffix.lower() == ".ores":
            parser.error("--json must not target an .ores source file")
        try:
            with args.json.open("x", encoding="utf-8") as report_file:
                json.dump(report, report_file, indent=2)
                report_file.write("\n")
        except OSError as exc:
            parser.error(f"cannot create new JSON report without overwrite: {exc}")
    return int(args.strict and bool(report["would_change"] or report["format_errors"]))


if __name__ == "__main__":
    sys.exit(main())
