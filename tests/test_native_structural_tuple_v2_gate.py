#!/usr/bin/env python3
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

HARNESS = Path(__file__).with_name("native_structural_tuple_v2_gate.py")
MOCK = r'''#!/usr/bin/env python3
from pathlib import Path
import sys
mode=Path(sys.argv[0]).name
src=Path(sys.argv[-1]).read_text()
if "pub fnc main(): int { return 42; }" in src:
    if mode=="always-reject":
        print("bad",file=sys.stderr);sys.exit(1)
    print("define i64 @__oreslang_guest_main(");sys.exit(0)
if mode=="always-accept":
    print("define i64 @__oreslang_guest_main(");sys.exit(0)
if mode=="artifact":
    Path("leak.ll").write_text("bad")
if mode=="stdout":
    print("partial ir")
if "structural (" in src:
    print("native structural positional tuple requires verified nonescaping readonly-view lowering",file=sys.stderr)
else:
    print("native positional tuple parameter requires verified readonly-slot and optional-tail lowering",file=sys.stderr)
sys.exit(1)
'''

class HarnessTests(unittest.TestCase):
    def test_false_green_modes_are_rejected(self):
        for mode in ("correct","always-reject","always-accept","artifact","stdout"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as td:
                fake=Path(td)/mode
                fake.write_text(MOCK,encoding="utf-8")
                fake.chmod(0o700)
                r=subprocess.run([sys.executable,str(HARNESS),str(fake)],
                    cwd=td,text=True,capture_output=True,timeout=45,check=False)
                self.assertEqual(r.returncode==0, mode=="correct",
                                 (mode,r.returncode,r.stderr[-500:]))

if __name__=="__main__":
    unittest.main()
