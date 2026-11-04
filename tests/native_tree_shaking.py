#!/usr/bin/env python3
"""Executed proof for the first Oreslang LLVM tree-shaking slice.

The opt-in compiler pass is intentionally conservative: main and pub functions
are roots, and direct same-module calls are followed transitively. This test
proves removal in generated LLVM IR and behavior equivalence after native link.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile

def require(value, message):
    if not value:
        raise RuntimeError(message)

def run(argv):
    return subprocess.run(argv, stdin=subprocess.DEVNULL, capture_output=True,
                          timeout=30, check=False)

SOURCE = """pub fnc main(): int {
  return live(40);
}
fnc live(int value): int {
  return value + 2;
}
fnc dead(): int {
  return 99;
}
pub fnc exported_unused(): int {
  return 7;
}
"""

PRIVATE_ONLY = """fnc hidden(): int {
  return 1;
}
"""

CLASS_SURFACE = """define class Pair as
  pub final int a: start + 1;
  pub final mut int b: start * 2;
  constructor(int start) { return; }
end
pub fnc main(): int {
  let Pair p = new Pair(13);
  return p.a + p.b;
}
"""

ACTOR_SURFACE = """define actor Worker as {
  let total: int = 0;
  pub fnc on_message(int n): int {
    total = total + n;
    return total;
  }
}
pub fnc main(): int {
  const worker = spawn Worker();
  worker.send(1);
  stop worker;
  return 42;
}
"""

SCALAR_CONTINUATION_SURFACE = """pub routine count_to_one_thousand(): int {
  val count: int = 0;
  while (count < 1000) {
    count = count + 1;
  }
  return count;
}
"""

TERNARY_CALL_SURFACE = """fnc branch_only(): int {
  return 42;
}
fnc dead(): int {
  return 9;
}
pub fnc main(): int {
  return true ? branch_only() : 0;
}
"""

PUBLIC_OVERLOAD_SURFACE = """pub fnc api(int value): int {
  return value;
}
fnc api(int left, int right): int {
  return left + right;
}
pub fnc main(): int {
  return api(42);
}
"""

def compile_ir(compiler, source, shaken, roots=(), compiler_args=()):
    with tempfile.TemporaryDirectory(prefix="oreslang-tree-shake-source-") as td:
        path=Path(td)/"case.ores"
        path.write_text(source,encoding="utf-8")
        argv=[str(compiler), *compiler_args]
        if shaken:
            argv.append("--tree-shake")
            argv.extend("--tree-shake-root="+root for root in roots)
        argv.append(str(path))
        return run(argv)

def main():
    p=argparse.ArgumentParser()
    p.add_argument("--compiler",type=Path,required=True)
    p.add_argument("--clang",type=Path,required=True)
    args=p.parse_args()
    require(args.compiler.is_file(),"compiler unavailable")
    require(args.clang.is_file(),"clang unavailable")

    normal=compile_ir(args.compiler,SOURCE,False)
    conservative=compile_ir(args.compiler,SOURCE,True)
    shaken=compile_ir(args.compiler,SOURCE,True,("main",))
    require(normal.returncode==0,"unshaken compile failed: "+repr(normal.stderr[:1024]))
    require(conservative.returncode==0,"conservative tree-shaken compile failed: "+repr(conservative.stderr[:1024]))
    require(shaken.returncode==0,"exact-root tree-shaken compile failed: "+repr(shaken.stderr[:1024]))

    for symbol in (b"@live(",b"@dead(",b"@exported_unused("):
        require(symbol in normal.stdout,"unshaken IR missing expected symbol "+repr(symbol))
    require(b"@live(" in conservative.stdout,"reachable private helper was removed")
    require(b"@exported_unused(" in conservative.stdout,"conservative pub export root was removed")
    require(b"@dead(" not in conservative.stdout,"unreachable private function survived conservative tree shaking")
    require(b"@live(" in shaken.stdout,"reachable private helper was removed from exact-root build")
    require(b"@exported_unused(" not in shaken.stdout,"unused pub function survived exact executable roots")
    require(b"@dead(" not in shaken.stdout,"unreachable private function survived exact-root tree shaking")
    require(b"define i32 @main(" in shaken.stdout,"native entry wrapper missing")

    with tempfile.TemporaryDirectory(prefix="oreslang-tree-shake-link-") as td:
        d=Path(td)
        observed=[]
        for name,payload in (("normal",normal.stdout),("conservative",conservative.stdout),("shaken",shaken.stdout)):
            ir=d/(name+".ll"); exe=d/name
            ir.write_bytes(payload)
            linked=run([str(args.clang),"-O2","-x","ir",str(ir),"-o",str(exe)])
            require(linked.returncode==0,"clang link failed for "+name+": "+repr(linked.stderr[:1024]))
            result=run([str(exe)])
            require(result.returncode==42 and result.stdout==b"" and result.stderr==b"",
                    name+" behavior mismatch")
            observed.append(exe.stat().st_size)
        # Size is recorded as evidence, not a universal threshold: ELF/linker
        # padding can make tiny examples equal in file bytes.
        print("TREE_SHAKE_NATIVE_OK normal_bytes=%d shaken_bytes=%d" %
              (observed[0],observed[1]))

    rejected=compile_ir(args.compiler,PRIVATE_ONLY,True)
    require(rejected.returncode!=0 and rejected.stdout==b"" and
            b"requires an explicit root, main, or at least one pub function" in rejected.stderr,
            "rootless strict tree shaking did not fail closed")
    missing=compile_ir(args.compiler,SOURCE,True,("missing",))
    require(missing.returncode!=0 and missing.stdout==b"" and
            b"unknown tree-shake root" in missing.stderr,
            "unknown explicit tree-shake root did not fail closed")

    ternary=compile_ir(args.compiler,TERNARY_CALL_SURFACE,True)
    require(ternary.returncode==0,
            "ternary tree-shaken compile failed: "+repr(ternary.stderr[:1024]))
    require(b"@branch_only(" in ternary.stdout,
            "call reachable only from ternary arm was removed")
    require(b"@dead(" not in ternary.stdout,
            "unreachable function survived ternary tree shaking")

    overload_control=compile_ir(args.compiler,PUBLIC_OVERLOAD_SURFACE,False)
    overload_shaken=compile_ir(args.compiler,PUBLIC_OVERLOAD_SURFACE,True)
    require(overload_control.returncode==0 and overload_shaken.returncode==0,
            "public overload compile failed")
    for symbol in (b"@api$arity1", b"@api$arity2"):
        # LLVM versions may quote identifiers containing '$'. Compare exact
        # callable names in either spelling, not a prefix of another overload.
        spellings = (symbol+b"(", b'@"'+symbol[1:]+b'"(')
        require(any(name in overload_control.stdout for name in spellings),
                "control IR missing overload ABI symbol "+repr(symbol))
        require(any(name in overload_shaken.stdout for name in spellings),
                "conservative tree shaking changed public overload ABI "+repr(symbol))

    class_control=compile_ir(args.compiler,CLASS_SURFACE,False)
    class_shaken=compile_ir(args.compiler,CLASS_SURFACE,True)
    require(class_control.returncode==0,
            "class control compile failed: "+repr(class_control.stderr[:1024]))
    require(class_shaken.returncode!=0 and class_shaken.stdout==b"" and
            b"rejects class declarations until class initializer roots are modeled" in class_shaken.stderr,
            "class tree shaking did not fail closed")

    actor_control=compile_ir(args.compiler,ACTOR_SURFACE,False)
    actor_shaken=compile_ir(args.compiler,ACTOR_SURFACE,True)
    require(actor_control.returncode==0,
            "actor control compile failed: "+repr(actor_control.stderr[:1024]))
    require(actor_shaken.returncode!=0 and actor_shaken.stdout==b"" and
            b"rejects actor declarations until actor roots are modeled" in actor_shaken.stderr,
            "actor tree shaking did not fail closed")

    continuation_control=compile_ir(
        args.compiler,SCALAR_CONTINUATION_SURFACE,False,
        compiler_args=("--experimental-rt-scalar-loop",))
    continuation_shaken=compile_ir(
        args.compiler,SCALAR_CONTINUATION_SURFACE,True,
        compiler_args=("--experimental-rt-scalar-loop",))
    require(continuation_control.returncode==0,
            "continuation control compile failed: "+
            repr(continuation_control.stderr[:1024]))
    require(continuation_shaken.returncode!=0 and
            continuation_shaken.stdout==b"" and
            b"rejects scalar continuation functions until continuation ABI roots are modeled"
            in continuation_shaken.stderr,
            "scalar continuation tree shaking did not fail closed")
    return 0

if __name__=="__main__":
    try:
        raise SystemExit(main())
    except (OSError,RuntimeError,subprocess.SubprocessError) as error:
        print("LLVM TREE SHAKING DENIED: "+str(error),file=sys.stderr)
        raise SystemExit(1)
