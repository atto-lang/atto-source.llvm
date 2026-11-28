"""Native representative IR differential test; NOT actual Atto compiler evidence."""
import ctypes as C
import itertools
from pathlib import Path
import random
import signal
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parent
base = (root / 'representative_rem_mod.ll').read_text()
rem = (base[:base.index('  %neg =')] + '  ret i64 %rem\n}\n').replace('@modulo(', '@remainder(')
minimum, maximum = -(1 << 63), (1 << 63) - 1
edges = [minimum, minimum + 1, -9007199254740993, -9007199254740992, -47, -5, -2, -1, 0, 1, 2, 5, 47, 9007199254740992, 9007199254740993, maximum - 1, maximum]
rng = random.Random(20261126)
pairs = [(a, b) for a, b in itertools.product(edges, repeat=2) if b]
pairs += [(rng.randint(minimum, maximum), rng.randint(minimum, maximum) or 1) for _ in range(10000)]
with tempfile.TemporaryDirectory() as tmp:
    d = Path(tmp)
    (d/'mod.ll').write_text(base)
    (d/'rem.ll').write_text(rem)
    for opt in (0, 2):
        lib = d / f'arithmetic-O{opt}.so'
        subprocess.run(['clang', f'-O{opt}', '-shared', '-fPIC', str(d/'mod.ll'), str(d/'rem.ll'), '-o', str(lib)], check=True, capture_output=True)
        dll = C.CDLL(str(lib))
        for symbol in ('remainder', 'modulo'):
            f = getattr(dll, symbol); f.argtypes = [C.c_int64]*2; f.restype = C.c_int64
            for a, b in pairs:
                # Compute truncating remainder independently of floating point and C division.
                magnitude = abs(a) % abs(b)
                expected = (-magnitude if a < 0 else magnitude) if symbol == 'remainder' else a % abs(b)
                actual = f(a,b)
                if actual != expected: raise AssertionError((opt, symbol, a, b, actual, expected))
            for a in (0, minimum, maximum):
                child = '''import ctypes as C,resource,sys\nresource.setrlimit(resource.RLIMIT_CORE,(0,0))\nf=getattr(C.CDLL(sys.argv[1]),sys.argv[2]);f.argtypes=[C.c_int64]*2;f.restype=C.c_int64;f(int(sys.argv[3]),0)\n'''
                result = subprocess.run([sys.executable, '-c', child, str(lib), symbol, str(a)], capture_output=True, timeout=5)
                if result.returncode != -signal.SIGILL: raise AssertionError((opt, symbol, a, result.returncode))
            print(f'O{opt} {symbol}: {len(pairs)} exact oracle comparisons + 3 terminal zero traps passed')
print(f'TOTAL {4*(len(pairs)+3)} native checks passed (representative IR only)')
