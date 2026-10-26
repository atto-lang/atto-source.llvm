# Native inverted source must remain fail-closed until typed lowering exists.
import argparse, pathlib, subprocess, tempfile

p=argparse.ArgumentParser(); p.add_argument('--compiler',required=True); a=p.parse_args()
cases=[
('root','define inverted class Particle as x: int; end\n'),
('define-pub','define pub inverted class Particle as x: int; end\n'),
('pub-define','pub define inverted class Particle as x: int; end\n'),
('module','define module Physics as\n define inverted class Particle as x: int; end\n pub fnc main(): int { return 0; }\nend\n')]
with tempfile.TemporaryDirectory() as d:
 f=pathlib.Path(d)/'x.ores'
 for label,src in cases:
  f.write_text(src,encoding='utf-8')
  r=subprocess.run([a.compiler,str(f)],capture_output=True,text=True,timeout=40)
  if r.returncode==0: raise AssertionError(label+' unexpectedly compiled')
  if r.stdout.lstrip().startswith('; ModuleID'): raise AssertionError(label+' leaked LLVM IR')
  if not r.stderr.strip(): raise AssertionError(label+' lacked diagnostic')
print('PASS native inverted source fail-closed')
