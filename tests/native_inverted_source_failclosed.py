# Native compiler must reject guest inverted declarations until HIR/MIR
# checked column ownership and lowering are executable.
import argparse
import pathlib
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
args = parser.parse_args()
cases = [
    ("define inverted class Particle as x: int; end\n",
     "native inverted class requires typed one-level SoA ownership"),
    ("define pub inverted class Particle as x: int; end\n",
     "native inverted class requires typed one-level SoA ownership"),
    ("pub define inverted class Particle as x: int; end\n",
     "native inverted class requires typed one-level SoA ownership"),
    ("define inverted struct Position as x: int; y: int; end\n",
     "native inverted struct schema parsed but typed SoA HIR/MIR ownership lowering is not implemented"),
    ("define pub inverted struct Position as x: int; end\n",
     "native inverted struct schema parsed but typed SoA HIR/MIR ownership lowering is not implemented"),
    ("pub define inverted struct Position as x: int; end\n",
     "native inverted struct schema parsed but typed SoA HIR/MIR ownership lowering is not implemented"),
]
with tempfile.TemporaryDirectory() as temp:
    source = pathlib.Path(temp) / "source.ores"
    for content, diagnostic in cases:
        source.write_text(content, encoding="utf-8")
        result = subprocess.run(
            [args.compiler, str(source)],
            capture_output=True, text=True, timeout=40, check=False,
        )
        if result.returncode == 0:
            raise AssertionError("unlowered inverted source compiled")
        if result.stdout.lstrip().startswith("; ModuleID"):
            raise AssertionError("rejection leaked LLVM IR")
        if diagnostic not in result.stderr:
            raise AssertionError(
                "wrong inverted-source diagnostic: " + repr(result.stderr)
            )
print("PASS: 6 native inverted declaration compiler rejections")
