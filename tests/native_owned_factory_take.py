#!/usr/bin/env python3
"""Actual native direct factory take; separate from full allocator policy admission."""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
DEFS = """define class Bag as
  val Array<int> values;
  pub size():int { return self.values.size; }
end
define class Other as
  val Array<int> values;
end
fnc make():Bag {
  val Array<int> items = new Array<int>();
  items.add(42);
  return new Bag(items);
}
fnc scalar():int { return 42; }
"""

def run(args):
    return subprocess.run(args, text=True, capture_output=True, timeout=120)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--clang", required=True)
    args = parser.parse_args()
    compiler = str(Path(args.compiler).resolve(strict=True))
    clang = str(Path(args.clang).resolve(strict=True))
    runtime = ROOT / "src/main/cpp/oreslang/runtime/NativeIntArrayRegion.c"
    with tempfile.TemporaryDirectory(prefix="owned-factory-") as root:
        root = Path(root)
        initializers = ["take! make()", "rt take make()", "take! (make())", "move! make()"]
        for spelling in ("take", "move"):
            for gap in ("\t", "\n", "\r\n", "\v", "\f", " /* gap */ "):
                initializers.append(spelling + "!" + gap + "make()")
        for i, initializer in enumerate(initializers):
            source = root / f"positive{i}.atto"
            source.write_text(DEFS + "pub fnc main():int { let mut Bag b = " + initializer +
                              "; let mut Bag moved = " + ("move!" if initializer.startswith("move!") else "take!") +
                              " b; return moved.size() == 1 ? 42 : 7; }\n")
            emitted = run([compiler, str(source)])
            assert emitted.returncode == 0 and "@main" in emitted.stdout, emitted.stderr
            ir = source.with_suffix(".ll")
            ir.write_text(emitted.stdout)
            exe = source.with_suffix(".bin")
            built = run([clang, "-O2", "-Wno-override-module", "-x", "ir", str(ir),
                         "-x", "c", str(runtime), "-I", str(ROOT / "src/main/cpp"), "-o", str(exe)])
            assert built.returncode == 0, built.stderr
            source.unlink()
            actual = run([str(exe)])
            assert actual.returncode == 42, actual
        negatives = [
            ("let mut Other b = take! make(); return 0;", "matching nominal class result"),
            ("let mut Bag b = take! scalar(); return 0;", "matching nominal class result"),
            ("let Bag b = take! make(); return 0;", "requires let mut owner"),
            ("let mut Bag b = take! make(); let mut Bag c = take! b; return b.size();", "move"),
            ("let mut Bag b = move! make(); let mut Bag c = move! b; return b.size();", "move"),
            ("let mut Other b = move! make(); return 0;", "matching nominal class result"),
            ("let Bag b = make(); return 0;", "owning class result requires"),
            ("let mut Bag b = take! make(); let Bag alias = b; return 0;", "immutable record value"),
        ]
        for spelling in ("take", "move"):
            for gap in ("", "/*gap*/", "//gap\n", "\u00a0"):
                negatives.append(("let mut Bag b = " + spelling + "!" + gap + "make(); return 0;",
                                  "E_BANG_KEYWORD_WHITESPACE"))
            for gap in (" ", "/*gap*/", "\n"):
                negatives.append(("let mut Bag b = " + spelling + gap + "! make(); return 0;",
                                  "E_BANG_KEYWORD_WHITESPACE"))
            negatives.append(("let mut Bag b = " + spelling + "!(make()); return 0;",
                              "E_BANG_KEYWORD_WHITESPACE"))
        # Preserve fail-closed ownership admission before scoped class cleanup exists.
        negatives.append(("if true; do let mut Bag b = take! make(); fi return 0;",
                          "native branch-local class owner requires destructor/drop lowering"))
        for i, (body, diagnostic) in enumerate(negatives):
            source = root / f"negative{i}.atto"
            source.write_text(DEFS + "pub fnc main():int { " + body + " }\n")
            denied = run([compiler, str(source)])
            assert 0 < denied.returncode < 128 and not denied.stdout.strip(), denied
            assert diagnostic in denied.stderr, denied.stderr
    print(f"OWNED_FACTORY_TAKE: {len(initializers)} native executions, {len(negatives)} targeted denials")

if __name__ == "__main__":
    main()
