#!/usr/bin/env python3
"""Build guest IR and C/C++ adapter together; execute over real TCP sockets."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile
p = argparse.ArgumentParser()
p.add_argument('--compiler', required=True)
p.add_argument('--clang', required=True)
p.add_argument('--cxx', required=True)
p.add_argument('--root', type=Path, required=True)
p.add_argument('--sanitize', action='store_true')
a = p.parse_args()
with tempfile.TemporaryDirectory(prefix='ores-native-http-') as temporary:
    env = dict(os.environ, ORESLANG_LLVMC=str(Path(a.compiler).resolve()), CLANG=a.clang, CXX=a.cxx,
               SANITIZE='1' if a.sanitize else '0')
    subprocess.run(['bash', str(a.root / 'scripts/build-native-http.sh'), temporary], env=env, check=True, timeout=90)
    subprocess.run([sys.executable, str(a.root / 'tests/native_http_actor_e2e.py'),
                    '--server', str(Path(temporary) / 'native-http-server')], check=True, timeout=60)
