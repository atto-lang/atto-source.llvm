#!/usr/bin/env python3
"""No-eval flags2env wrapper regression using a deterministic mock executable.

This tests the wrapper's trust boundary; it is not upstream flags2env parsing
or full GraalVM/LLVM compilation evidence.
"""
import argparse
import os
import pathlib
import subprocess
import sys
import tempfile

arg = argparse.ArgumentParser()
arg.add_argument("--root", type=pathlib.Path, required=True)
args = arg.parse_args()
wrapper = args.root.resolve() / "scripts/with-flags2env.sh"

with tempfile.TemporaryDirectory(prefix="ores-f2e-hardening-") as work:
    directory = pathlib.Path(work)
    marker = directory / "unexpected"
    fake = directory / "mock-flags2env"
    fake.write_text(
        "#!" + sys.executable + "\n"
        "import os,sys\n"
        "sys.stdout.write(os.environ.get('MOCK_F2E_OUTPUT',''))\n"
        "sys.stderr.write(os.environ.get('MOCK_F2E_STDERR',''))\n"
        "sys.exit(int(os.environ.get('MOCK_F2E_STATUS','0')))\n",
        encoding="utf-8",
    )
    fake.chmod(0o700)

    # Underlying 'guest' prints its argv and selected env. No shell is used.
    child = (
        "import os,sys; "
        "print(os.environ.get('ORES_MODE','-')); "
        "print(os.environ.get('ORES_CHECK','-')); "
        "print(repr(sys.argv[1:]))"
    )

    def run(value, status=0, stderr=""):
        env = dict(os.environ)
        env.update(
            FLAGS2ENV_BIN=str(fake),
            MOCK_F2E_OUTPUT=value,
            MOCK_F2E_STATUS=str(status),
            MOCK_F2E_STDERR=stderr,
        )
        return subprocess.run(
            ["bash", str(wrapper), sys.executable, "-c", child, "source file.ores"],
            cwd=work, capture_output=True, text=True, timeout=20, env=env,
        )

    accepted = run("export ORES_MODE='a b'\nexport ORES_CHECK='true'\n")
    assert accepted.returncode == 0, accepted.stderr
    assert accepted.stdout.splitlines() == ["a b", "true", "['source file.ores']"]

    literal = run(f"export ORES_MODE='$(touch {marker})'\n")
    assert literal.returncode == 0, literal.stderr
    assert "$(touch" in literal.stdout
    assert not marker.exists(), "shell-substitution must never execute"

    for output in (
        "export PATH='/tmp/evil'\n",
        "export ORES_BOGUS='yes'\n",
        "export ORES_CHECK='true'\nexport ORES_CHECK='false'\n",
        f"export ORES_MODE='x'; touch {marker}\n",
        "echo unexpected\n",
        "export ORES_CHECK='unterminated\n",
        "export ORES_MODE='a\\nb'\n",
    ):
        rejected = run(output)
        assert rejected.returncode != 0, (output, rejected.stdout, rejected.stderr)
        assert not marker.exists(), "rejected shell text must not execute the guest"

    rejected = run("export ORES_CHECK='true'\n", status=2)
    assert rejected.returncode == 2
    rejected = run("export ORES_CHECK='true'\n", stderr="warning")
    assert rejected.returncode != 0
    print("flags2env wrapper no-eval, argv fidelity, and rejection matrix passed")
