#!/usr/bin/env python3
"""Source-free adversarial self-test of the native structural gate, NOT LLVM parity."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HARNESS = Path(__file__).with_name("native_structural_fail_closed.py")
MOCK = '''#!/usr/bin/env python3
from pathlib import Path
import sys
mode = Path(sys.argv[0]).name
source = Path(sys.argv[-1]).read_text(encoding="utf-8")
scalar = "pub fnc main(): int { return 42; }" in source
if mode == "always-reject":
    print("unsupported", file=sys.stderr)
    sys.exit(1)
if mode == "always-accept":
    print("define i64 @__oreslang_guest_main(")
    sys.exit(0)
if scalar:
    print("define i64 @__oreslang_guest_main(")
    sys.exit(0)
if mode == "artifact":
    Path("partial-output.ll").write_text("bad")
if mode == "stdout-leak":
    print("partial LLVM IR")
if mode != "missing-diagnostic":
    print("oreslang-llvmc: unsupported structural source", file=sys.stderr)
sys.exit(1)
'''

class NativeStructuralGateAdversarialTests(unittest.TestCase):
    def test_real_harness_denies_false_green_compilers(self):
        for mode in ("correct", "always-reject", "always-accept",
                     "artifact", "stdout-leak", "missing-diagnostic"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as temp:
                fake = Path(temp) / mode
                fake.write_text(MOCK, encoding="utf-8")
                fake.chmod(0o700)
                result = subprocess.run(
                    [sys.executable, str(HARNESS), str(fake)],
                    cwd=temp, text=True, capture_output=True, timeout=45, check=False)
                self.assertEqual(result.returncode == 0, mode == "correct",
                                 (mode, result.returncode, result.stderr[-600:]))

if __name__ == "__main__":
    unittest.main()
