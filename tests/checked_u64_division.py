import os, subprocess as s, tempfile, pathlib, hashlib, resource
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
P=pathlib.Path(os.environ['ATTO_U64_DIV_SOURCE']).resolve(); src=P.read_bytes()
assert hashlib.sha1(b'blob '+str(len(src)).encode()+b'\0'+src).hexdigest()=='0fd2d7bf59912bd9182261bd4b3182da1655fa5e'
M=(1<<64)-1
cases=[('7/3',2),('1+8/3*2',5),('20/3/2',3),('20/(3/2)',20),(f'{M}/1',M),(f'{M}/2',M//2),('9223372036854775808/9223372036854775809',0),(f'0/{M}',0),('1/0','zero'),('0/0','zero'),('0*(1/0)','zero'),(f'(1/0)+({M}+1)','zero'),('first(42,1/0)','zero'),(f'({M}+1)/0','overflow'),('1/(0-1)','overflow'),('(0-1)+(1/0)','overflow')]
with tempfile.TemporaryDirectory() as tmp:
 os.chdir(tmp)
 def run(cmd,inp=None,expect=0):
  r=s.run(cmd,input=inp,text=True,capture_output=True,timeout=30)
  assert r.returncode==expect,(cmd,r.returncode,r.stderr)
  return r.stdout
 run(['clang++','-std=c++20','-O2','-Wall','-Wextra','-Werror',str(P),'-o','frontend'])
 def lower(e,mode):return run(['./frontend','--emit='+mode],f'pub fnc main():uint{{return {e};}} fnc first(u64 a,u64 b):u64{{return a;}}')
 pathlib.Path('observer.c').write_text('#include <stdint.h>\n#include <inttypes.h>\n#include <stdio.h>\nextern uint64_t atto_guest_main(void);\nint main(void){printf("%" PRIu64 "\\n",atto_guest_main());}\n')
 run(['clang','-c','observer.c','-o','observer.o'])
 pathlib.Path('wasm.cjs').write_text('WebAssembly.instantiate(require("fs").readFileSync("guest.wasm")).then(({instance})=>{try{console.log(BigInt.asUintN(64,instance.exports.atto_guest_main()).toString())}catch(e){if(!(e instanceof WebAssembly.RuntimeError))throw e;process.exit(71)}})')
 for mode in ['llvm','forge','wasm']:
  for i,(expr,want) in enumerate(cases):
   opt='-O0' if i%2==0 else '-O2'; artifact=lower(expr,'cpp' if mode=='forge' else 'llvm')
   if mode=='forge':run(['clang++','-std=c++20',opt,'-fsanitize=undefined','-fno-sanitize-recover=all','-x','c++','-','-o','guest'],artifact)
   elif mode=='llvm':run(['clang','-Wno-override-module',opt,'-x','ir','-','-x','none','observer.o','-o','guest'],artifact)
   else:run(['clang','--target=wasm32-unknown-unknown','-Wno-override-module',opt,'-nostdlib','-Wl,--no-entry','-Wl,--export=atto_guest_main','-x','ir','-','-o','guest.wasm'],artifact)
   r=s.run(['node','wasm.cjs'] if mode=='wasm' else ['./guest'],capture_output=True,text=True,timeout=3)
   if isinstance(want,int):assert (r.returncode,r.stdout,r.stderr)==(0,str(want)+'\n',''),(mode,expr,r)
   else:
    assert not r.stdout
    if mode=='forge':assert (r.returncode,r.stderr)==((71,'E_U64_DIVISION_BY_ZERO\n') if want=='zero' else (70,'E_U64_OVERFLOW\n'))
    else:assert r.returncode==(-4 if mode=='llvm' else 71),(mode,expr,r)
  print(mode, '16 compiled guests passed',flush=True)
 # Export only the emitted helper for dynamic test inputs.
 ir=lower('1/1','llvm').replace('define internal i64 @checked_div(', 'define i64 @checked_div(')
 pathlib.Path('helper.ll').write_text(ir)
 for opt in ['-O0','-O2']:
  run(['clang','--target=wasm32-unknown-unknown','-Wno-override-module',opt,'-nostdlib','-Wl,--no-entry','-Wl,--export=checked_div','helper.ll','-o','helper.wasm'])
  pathlib.Path('dynamic.cjs').write_text('const assert=require("node:assert/strict");const M=(1n<<64n)-1n;let state=0x9e3779b97f4a7c15n;const next=()=>{state^=(state<<13n)&M;state^=state>>7n;state^=(state<<17n)&M;return state};WebAssembly.instantiate(require("fs").readFileSync("helper.wasm")).then(({instance})=>{const d=instance.exports.checked_div;for(let i=0;i<256;i++){const a=next(),r=next(),b=i%8===0?0n:i%8===1?1n:i%8===2?M:i%8===3?2n:i%8===4?r&0xffffffffn:r;if(b===0n)assert.throws(()=>d(a,b),WebAssembly.RuntimeError);else assert.equal(BigInt.asUintN(64,d(a,b)),a/b);}console.log("256 dynamic inputs passed")})')
  print(opt,run(['node','dynamic.cjs']).strip(),flush=True)
  for target in ['i386-linux-gnu','armv7-none-eabi','thumbv7m-none-eabi','aarch64-linux-gnu','riscv32-unknown-elf','riscv64-unknown-elf']:
   run(['clang','--target='+target,'-Wno-override-module',opt,'-ffreestanding','-c','helper.ll','-o','target.o'])
   for line in run(['readelf','-Ws','target.o']).splitlines():
    if ' UND ' in line:assert line.split()[-1] in ['UND','_GLOBAL_OFFSET_TABLE_','__aeabi_unwind_cpp_pr0','__aeabi_unwind_cpp_pr1'],(target,opt,line)
 print('12 cross-target objects passed',flush=True)
 for e in ['1//2','1/*x*/2','1%2','1/-2','1/18446744073709551616']:
  r=s.run(['./frontend'],input=f'pub fnc main():u64{{return {e};}}',capture_output=True,text=True);assert r.returncode==2 and not r.stdout and 'E_U64_PROFILE' in r.stderr
 assert 'checked_div' not in lower('2*3','llvm') and 'checked_div' not in lower('2*3','cpp')
 print('five rejects and nondivision helper omission passed')
