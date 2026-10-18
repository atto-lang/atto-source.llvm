#!/usr/bin/env python3
"""Fail-closed native boundary for private-abstract/class virtual slots."""
import argparse
from pathlib import Path
import subprocess
import tempfile

DIAG = "native private abstract method slots unsupported"
ACTOR_DIAG = "native actor receive/private abstract slots unsupported"

REJECTIONS = {
    "abstract-class-explicit-fnc": ("""define abstract class Framework as
  private abstract fnc receive(int value): int;
end
pub fnc main(): int { return 0; }
""", DIAG),
    "class-private-abstract-fnc": ("""define class Framework as
  private abstract fnc receive(int value): int;
end
pub fnc main(): int { return 0; }
""", DIAG),
    "class-private-abstract-shorthand": ("""define class Framework as
  private abstract receive(int value): int;
end
pub fnc main(): int { return 0; }
""", DIAG),
    "generic-abstract-class": ("""define abstract class Framework<T> as
  private abstract fnc receive(T value): T;
end
pub fnc main(): int { return 0; }
""", DIAG),
    "actor-explicit-self-receive": ("""define actor Worker as
  let state: int = 0;
  receive(self, ActorMail<int> mail): void { return; }
end
pub fnc main(): int { return 0; }
""", ACTOR_DIAG),
    "actor-implicit-self-receive": ("""define actor Worker as
  let state: int = 0;
  receive(ActorMail<int> mail): void { return; }
end
pub fnc main(): int { return 0; }
""", ACTOR_DIAG),
    "actor-private-abstract-receive": ("""define actor Worker as
  let state: int = 0;
  private abstract receive(ActorMail<int> mail): void;
end
pub fnc main(): int { return 0; }
""", ACTOR_DIAG),
}

POSITIVE = """pub fnc main(): int {
  return 42;
}
"""

def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=60)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--compiler", required=True)
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="ores-private-abstract-") as root:
        root = Path(root)

        positive = root / "scalar-positive.ores"
        positive.write_text(POSITIVE, encoding="utf-8")
        compiled = run([args.compiler, str(positive)])
        if compiled.returncode != 0 or compiled.stderr:
            raise AssertionError(("positive control", compiled.returncode, compiled.stderr))
        if not compiled.stdout.startswith("; ModuleID"):
            raise AssertionError(("positive control emitted no LLVM IR", compiled.stdout[:200]))

        for name, (source, diagnostic) in REJECTIONS.items():
            path = root / f"{name}.ores"
            path.write_text(source, encoding="utf-8")
            before = {p.name for p in root.iterdir()}
            result = run([args.compiler, str(path)])
            after = {p.name for p in root.iterdir()}
            if result.returncode == 0:
                raise AssertionError((name, "unsupported source was accepted"))
            if result.stdout.strip():
                raise AssertionError((name, "rejection leaked partial IR/stdout", result.stdout[:300]))
            if diagnostic not in result.stderr:
                raise AssertionError((name, diagnostic, result.stderr))
            unexpected = after - before
            if unexpected:
                raise AssertionError((name, "rejection staged artifacts", sorted(unexpected)))

    print(f"PASS: scalar positive control + {len(REJECTIONS)} private-abstract/actor no-IR rejections")

if __name__ == "__main__":
    main()
