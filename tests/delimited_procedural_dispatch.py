"""Native compiler + linked guest checks for the restricted scalar subject slot."""
import pathlib
import subprocess
import sys
import tempfile

compiler, clang = sys.argv[1:]
contract = pathlib.Path(__file__).with_name("contract.py")
cases = [
    ("match-snapshot", "pub fnc main(): int { const int v = 7; do match |v * 6| over on 0 -> { return 0; } on 42 -> { return 42; } on _ -> { return 99; } end; return 0; }", ["--run", "42"]),
    ("switch-snapshot", "pub fnc main(): int { do switch |6 * 7| over case 1 -> { return 1; } case 42 -> { return 42; } default -> { return 99; } end; return 0; }", ["--run", "42"]),
    ("default-only", "pub fnc main(): int { do match |6 * 7| over on _ -> { return 42; } end; return 0; }", ["--run", "42"]),
    ("switch-continuation", "pub fnc main(): int { let int n = 0; do switch |true| over case true -> { n = 42; } default -> { n = 99; } end; return n; }", ["--run", "42"]),
    ("missing-subject", "pub fnc main(): int { do match |missing_subject| over on _ -> { return 42; } end; return 0; }", ["--diagnostic", "unknown parameter"]),
    ("fallback-order", "pub fnc main(): int { do match |7| over on _ -> { return 42; } on 7 -> { return 1; } end; return 0; }", ["--diagnostic", "native delimited match fallback must be last"]),
    ("call-pending", "fnc subject(): int { return 7; } pub fnc main(): int { do match |subject()| over on _ -> { return 42; } end; return 0; }", ["--diagnostic", "native procedural dispatch subject must be a replay-safe scalar expression"]),
]
with tempfile.TemporaryDirectory(prefix="atto-native-dispatch-") as tmp:
    for name, source, args in cases:
        fixture = pathlib.Path(tmp) / (name + ".ores")
        fixture.write_text(source + "\n", encoding="utf-8")
        subprocess.run([sys.executable, str(contract), "--compiler", compiler,
                        "--fixture", str(fixture), *args, "--clang", clang], check=True)
        print("PASS", name, flush=True)
