#!/usr/bin/env python3
"""Source bool stays distinct from int across loans, objects and module linking."""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

FLAG = '''pub define class Flag as
  let bool ready=true;
  pub read():bool{return self.ready;}
  pub set(mut self)(bool value):void{self.ready=value;return;}
end
'''


def run(command):
    return subprocess.run(command, capture_output=True, text=True, timeout=45)


def require(condition, evidence):
    if not condition:
        raise AssertionError(evidence)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--compiler', required=True)
    parser.add_argument('--clang', required=True)
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--java-command', nargs='+')
    args = parser.parse_args()
    positive = [
        ('literal-not', 'pub fnc main():int{const flag:bool=!!!false;if(flag){return 42;}fi return 0;}'),
        ('borrowed-false', 'fnc read(borrow bool flag):bool{return rt copy flag;}pub fnc main():int{const flag:bool=false;const result:bool=read(rt borrow flag);if(!result){return 42;}fi return 0;}'),
        ('typed-let', 'pub fnc main():int{let bool flag=true;if(flag eq true){return 42;}fi return 0;}'),
        ('nested-call', 'fnc flip(bool flag):bool{return !flag;}pub fnc main():int{const flag:bool=flip(flip(true));if(flag){return 42;}fi return 0;}'),
        ('borrow-copy', 'fnc read(borrow bool flag):bool{return rt copy flag;}pub fnc main():int{const flag:bool=true;const result:bool=read(rt borrow flag);if(result){return 42;}fi return 0;}'),
        ('exclusive-reborrow', 'fnc read(borrow bool flag):bool{return rt copy flag;}fnc forward(borrow mut bool flag):bool{return read(rt borrow flag);}pub fnc main():int{val flag:bool=true;const result:bool=forward(rt mut borrow flag);if(result){return 42;}fi return 0;}'),
        ('class-method', FLAG+'fnc exercise(borrow mut Flag f):bool{f.set(false);return f.read();}pub fnc main():int{let mut Flag f=new Flag();const after:bool=exercise(rt mut borrow f);if(!after){return 42;}fi return 0;}'),
        ('final-field', 'define class Flag as const bool ready=true;pub read():bool{return self.ready;}end pub fnc main():int{let Flag f=new Flag();const flag:bool=f.read();if(flag){return 42;}fi return 0;}'),
        ('bool-guarded-result', 'fnc flag(int n):bool{if(n eq 1){return true;}fi return false;}pub fnc main():int{const result:bool=flag(1);if(result){return 42;}fi return 0;}'),
        ('branch-write', FLAG+'pub fnc main():int{let mut Flag f=new Flag();if(true eq false){f.set(false);}else{f.set(true);}fi return 42;}'),
        ('branch-local-bool', FLAG+'fnc exercise(borrow mut Flag f):bool{if(true eq false){const ready:bool=false;f.set(ready);}else{const ready:bool=true;f.set(ready);}fi return f.read();}pub fnc main():int{let mut Flag f=new Flag();const ready:bool=exercise(rt mut borrow f);if(ready){return 42;}fi return 0;}'),
        ('take-local', 'pub fnc main():int{val original:bool=true;const moved:bool=rt take original;if(moved){return 42;}fi return 0;}'),
    ]
    # Bind the same exact source bytes used by Java/.NET/Cranelift/Wasm.
    shared_five_backend = Path(__file__).with_name('scalar-cast-inference-five-backend.ores')
    positive.append(('five-backend-byte-identical-fixture', shared_five_backend.read_text()))

    # These are native Oreslang guest programs, not host-language arithmetic
    # stand-ins. All accepted programs must emit IR, link and exit 42.
    positive.extend([
        ('five-backend-type-first-immutable', 'pub fnc main():int{int x=7;const y=x;const int z=y as int;return z+35;}'),
        ('type-first-bool-immutable', 'pub fnc main():int{bool flag=true;if(flag){return 42;}fi return 0;}'),
        ('type-first-int-expression', 'pub fnc main():int{int x=(2+5) as int;return x+35;}'),
        ('inferred-int', 'pub fnc main():int{const x=7;return x+35;}'),
        ('explicit-vs-cast', 'pub fnc main():int{const int a=7;const b=7 as int;return a+b+28;}'),
        ('annotation-vs-cast', 'pub fnc main():int{const a:int=7;const b=7 as int;return a+b+28;}'),
        ('parenthesized-cast', 'pub fnc main():int{const a=(2+5) as int;return a+35;}'),
        ('cast-then-add', 'pub fnc main():int{const a=(7 as int)+35;return a;}'),
        ('five-backend-expression-identity', 'pub fnc main():int{return (7 as int)+35;}'),
        ('five-backend-chained-identity', 'pub fnc main():int{return (7 as int as int)+35;}'),
        ('inferred-reference', 'pub fnc main():int{const a=7;const b=a as int;return b+35;}'),
        ('inferred-boolean', 'pub fnc main():int{const yes=true; if(yes){return 42;}fi return 0;}'),
        ('cast-boolean', 'pub fnc main():int{const yes=true as bool; if(yes){return 42;}fi return 0;}'),
        ('comparison-cast', 'pub fnc main():int{const yes=(7 eq 7) as bool;if(yes){return 42;}fi return 0;}'),
        ('conditional-inference', 'pub fnc main():int{const x=(true?7:8);return x+35;}'),
        ('conditional-cast', 'pub fnc main():int{const x=(true?7:8) as int;return x+35;}'),
        ('call-inference', 'fnc seven():int{return 7;}pub fnc main():int{const x=seven();return x+35;}'),
        ('call-cast', 'fnc seven():int{return 7;}pub fnc main():int{const x=seven() as int;return x+35;}'),
        ('branch-inference', 'pub fnc main():int{if(true){const x=7 as int;return x+35;}fi return 0;}'),
        ('scalar-alias-cast', 'declare type Code=int;pub fnc main():int{const x=7 as Code;return x+35;}'),
        ('scalar-alias-bool-cast', 'declare type Flag=bool;pub fnc main():int{const yes=true as Flag;if(yes){return 42;}fi return 0;}'),
        ('mutable-inferred-scalar', 'pub fnc main():int{let x=7 as int;x=42;return x;}'),
        ('chain-cast', 'pub fnc main():int{const x=7 as int as int;return x+35;}'),
        ('bool-chain-cast', 'pub fnc main():int{const x=true as bool as bool;if(x){return 42;}fi return 0;}'),
        ('inferred-val', 'pub fnc main():int{val x=7;const y=x as int;return y+35;}'),
        ('inferred-let', 'pub fnc main():int{let x=7;x=42;return x;}'),
        ('bool-let', 'pub fnc main():int{let x=false;x=true;if(x){return 42;}fi return 0;}'),
        ('inferred-bool-negation', 'pub fnc main():int{const x=!(false as bool);if(x){return 42;}fi return 0;}'),
        ('inferred-bool-call', 'fnc yes():bool{return true;}pub fnc main():int{const x=yes();if(x){return 42;}fi return 0;}'),
        ('cast-bool-call', 'fnc yes():bool{return true;}pub fnc main():int{const x=yes() as bool;if(x){return 42;}fi return 0;}'),
        ('cast-return-int', 'fnc seven():int{return 7 as int;}pub fnc main():int{return seven()+35;}'),
        ('inferred-multiple-hops', 'pub fnc main():int{const a=7;const b=a;const c=b as int;return c+35;}'),
        ('inferred-bool-multiple-hops', 'pub fnc main():int{const a=true;const b=a;const c=b as bool;if(c){return 42;}fi return 0;}'),
        ('cast-min-i64', 'pub fnc main():int{const x=(-9223372036854775808) as int;if(x<0){return 42;}fi return 0;}'),
        ('cast-max-i64', 'pub fnc main():int{const x=9223372036854775807 as int;if(x>0){return 42;}fi return 0;}'),
        ('bool-conditional-cast', 'pub fnc main():int{const x=(true?true:false) as bool;if(x){return 42;}fi return 0;}'),
        ('bool-to-bool-annotation', 'pub fnc main():int{const x:bool=true as bool;if(x){return 42;}fi return 0;}'),
    ])
    # The same four native .ores guest expressions are exercised by the Wasm
    # compiler-backed matrix; this lane must compile, link and return 42.
    positive.extend([
        ('dual-identity-operands', 'pub fnc main():int{return (7 as int)+(35 as int);}' ),
        ('grouped-identity-multiplication', 'pub fnc main():int{return ((2+5) as int)*6;}' ),
        ('chained-identity-multiplication', 'pub fnc main():int{return (7 as int as int)*6;}' ),
        ('identity-through-const-reference', 'pub fnc main():int{const x=7 as int;const y=x as int;return y*6;}' ),
    ])
    positive.extend([
        ('ssa-let-multiwrite', 'pub fnc main():int{let x=7;x=x+1;x=x+34;return x;}'),
        ('ssa-let-runtime-const-snapshot', 'pub fnc main():int{let x=7;const snap=x;x=42;return snap+35;}'),
        ('ssa-let-runtime-val-snapshot', 'pub fnc main():int{let x=7;val snap=x;x=42;return snap+35;}'),
        ('ssa-let-cast-snapshot', 'pub fnc main():int{let x=7;const snap=x as int;x=42;return snap+35;}'),
        ('ssa-let-casted-write', 'pub fnc main():int{let x=7 as int;x=(x+35) as int;return x;}'),
        ('ssa-let-from-immutable', 'pub fnc main():int{const base=7;let x=base;x=42;return x;}'),
    ])
    negative = [
        ('bool-as-int', 'pub fnc main():int{return true;}'),
        ('five-backend-int-to-bool', 'pub fnc main():int{return 7 as bool;}'),
        ('five-backend-bool-to-int', 'pub fnc main():int{return true as int;}'),
        ('five-backend-cast-overflow', 'pub fnc main():int{return 9223372036854775808 as int;}'),
        ('int-as-bool', 'fnc bad():bool{return 1;}pub fnc main():int{return 0;}'),
        ('local-int-as-bool', 'pub fnc main():int{const flag:bool=1;return 0;}'),
        ('local-bool-as-int', 'pub fnc main():int{const number:int=false;return 0;}'),
        ('truthy-int', 'pub fnc main():int{if(1){return 42;}fi return 0;}'),
        ('bool-arithmetic', 'pub fnc main():int{return true+1;}'),
        ('negated-bool', 'pub fnc main():int{return -true;}'),
        ('not-integer', 'fnc bad():bool{return !1;}pub fnc main():int{return 0;}'),
        ('argument', 'fnc read(bool flag):int{return 42;}pub fnc main():int{return read(1);}'),
        ('reverse-argument', 'fnc read(int flag):int{return 42;}pub fnc main():int{return read(false);}'),
        ('borrow-type', 'fnc read(borrow bool flag):bool{return rt copy flag;}pub fnc main():int{val number:int=1;const flag:bool=read(rt borrow number);return 0;}'),
        ('reverse-borrow-type', 'fnc read(borrow int flag):int{return rt copy flag;}pub fnc main():int{val flag:bool=true;return read(rt borrow flag);}'),
        ('comparison-type', 'pub fnc main():int{if(true eq 1){return 42;}fi return 0;}'),
        ('class-field-init', 'define class Flag as let bool ready=1;end pub fnc main():int{return 0;}'),
        ('class-field-write', FLAG+'pub fnc main():int{let mut Flag f=new Flag();f.set(1);return 0;}'),
        ('field-into-int', FLAG+'pub fnc main():int{let Flag f=new Flag();return f.read();}'),
        ('main-bool', 'pub fnc main():bool{return true;}'),
        ('actor-initializer', 'define actor Worker as let count:int=true;pub on_message(int n):int{return n;}end pub fnc main():int{return 0;}'),
        ('array-bool', 'pub fnc main():int{let mut Array<int> a=new Array<int>();a.add(true);return 0;}'),
        ('generic-bool', 'fnc id<T>(T n):T{return n;}pub fnc main():int{return id(true);}'),
    ]
    negative.extend([
        ('type-first-int-rejects-bool', 'pub fnc main():int{int x=true;return 0;}'),
        ('type-first-bool-rejects-int', 'pub fnc main():int{bool x=7;return 0;}'),
        ('cast-int-to-bool', 'pub fnc main():int{const x=7 as bool;return 0;}'),
        ('cast-bool-to-int', 'pub fnc main():int{const x=true as int;return 0;}'),
        ('cast-string-to-int', 'pub fnc main():int{const x="7" as int;return 0;}'),
        ('cast-int-to-string', 'pub fnc main():int{const x=7 as string;return 0;}'),
        ('cast-optional-no-lowering', 'pub fnc main():int{const x=7 as? int;return 0;}'),
        ('cast-nominal-no-lowering', 'pub fnc main():int{const x=7 as NotAType;return 0;}'),
        ('inferred-bool-into-int', 'pub fnc main():int{const x=true;const int y=x;return 0;}'),
        ('inferred-int-into-bool', 'pub fnc main():int{const x=7;const bool y=x;return 0;}'),
        ('cast-does-not-convert', 'pub fnc main():int{const x=7 as bool as int;return 0;}'),
        ('scalar-alias-cast-mismatch', 'declare type Flag=bool;pub fnc main():int{const x=7 as Flag;return 0;}'),
        ('cast-move-use-after-move', 'pub fnc main():int{val x=7;const moved=(rt take x) as int;return x;}'),
        ('inference-rejects-sum-handle', 'pub fnc main():int{const Option<int> wrapped=Some(7);const x=wrapped;return 0;}'),
        ('cast-rejects-sum-handle', 'pub fnc main():int{const Option<int> wrapped=Some(7);const int x=wrapped as int;return 0;}'),
        ('inferred-bool-arithmetic', 'pub fnc main():int{const x=true;return x+1;}'),
        ('cast-false-to-int', 'pub fnc main():int{const x=false as int;return 0;}'),
        ('cast-zero-to-bool', 'pub fnc main():int{const x=0 as bool;return 0;}'),
        ('cast-int-expression-to-bool', 'pub fnc main():int{const x=(2+5) as bool;return 0;}'),
        ('cast-bool-expression-to-int', 'pub fnc main():int{const x=(7 eq 7) as int;return 0;}'),
        ('cast-bool-call-to-int', 'fnc yes():bool{return true;}pub fnc main():int{const x=yes() as int;return 0;}'),
        ('cast-int-call-to-bool', 'fnc seven():int{return 7;}pub fnc main():int{const x=seven() as bool;return 0;}'),
        ('cast-optional-bool-no-lowering', 'pub fnc main():int{const x=true as? bool;return 0;}'),
        ('inferred-bool-let-int-write', 'pub fnc main():int{let x=true;x=7;return 0;}'),
        ('inferred-int-let-bool-write', 'pub fnc main():int{let x=7;x=false;return 0;}'),
        ('bool-cast-then-int', 'pub fnc main():int{const x=true as bool as int;return 0;}'),
        ('cast-int-as-bool-from-variable', 'pub fnc main():int{const x=7;const y=x as bool;return 0;}'),
    ])
    negative.extend([
        ('nested-identity-then-bool', 'pub fnc main():int{return (7 as int) as bool;}'),
        ('nested-bool-then-int', 'pub fnc main():int{return (false as bool) as int;}'),
        ('identity-then-optional', 'pub fnc main():int{return (7 as int) as? int;}'),
        ('identity-then-bool-arithmetic', 'pub fnc main():int{return (7 as int)+true;}'),
    ])
    negative.extend([
        ('exclusive-alias', 'fnc pair(borrow mut bool a,borrow mut bool b):int{return 42;}pub fnc main():int{val flag:bool=true;return pair(rt mut borrow flag,rt mut borrow flag);}'),
        ('shared-exclusive-alias', 'fnc pair(borrow bool a,borrow mut bool b):int{return 42;}pub fnc main():int{val flag:bool=true;return pair(rt borrow flag,rt mut borrow flag);}'),
        ('moved-bool', 'pub fnc main():int{val flag:bool=true;const moved:bool=rt take flag;if(flag){return 42;}fi return 0;}'),
        ('readonly-mutable-loan', 'fnc read(borrow mut bool flag):int{return 42;}pub fnc main():int{const flag:bool=true;return read(rt mut borrow flag);}'),
    ])
    ownership_diagnostics = {
        'exclusive-alias': 'conflicting active native borrow',
        'shared-exclusive-alias': 'conflicting active native borrow',
        'moved-bool': 'use of explicitly moved int',
        'readonly-mutable-loan': 'cannot mutably borrow immutable owner',
    }
    scalar_diagnostics = {
        'bool-as-int': 'native function return requires int, got bool',
        'int-as-bool': 'native function return requires bool, got int',
        'local-int-as-bool': 'native scalar local initializer requires bool, got int',
        'local-bool-as-int': 'native scalar local initializer requires int, got bool',
        'truthy-int': 'native guarded return condition requires bool, got int',
        'bool-arithmetic': 'arithmetic requires int, got bool',
        'negated-bool': "unary '-' requires int, got bool",
        'not-integer': "logical '!' requires bool, got int",
        'argument': "argument 'flag' requires bool, got int",
        'reverse-argument': "argument 'flag' requires int, got bool",
        'borrow-type': "argument 'flag' requires bool, got int",
        'reverse-borrow-type': "argument 'flag' requires int, got bool",
        'comparison-type': 'equality operands require the same scalar type',
        'class-field-write': "argument 'value' requires bool, got int",
        'field-into-int': 'native function return requires int, got bool',
        'array-bool': 'native Array<int> arguments require int, got bool',
    }
    ownership_diagnostics.update(scalar_diagnostics)
    ownership_diagnostics.update({
        'type-first-int-rejects-bool': 'native scalar local initializer requires int, got bool',
        'type-first-bool-rejects-int': 'native scalar local initializer requires bool, got int',
        'cast-int-to-bool': 'impossible native cast: int as bool',
        'cast-bool-to-int': 'impossible native cast: bool as int',
        'cast-string-to-int': 'impossible native cast: string as int',
        'cast-int-to-string': 'impossible native cast: int as string',
        'cast-optional-no-lowering': 'native as? requires Option<T> checked-cast lowering',
        'cast-nominal-no-lowering': 'native checked cast requires a scalar or checked scalar alias',
        'inferred-bool-into-int': 'native scalar local initializer requires int, got bool',
        'inferred-int-into-bool': 'native scalar local initializer requires bool, got int',
        'cast-does-not-convert': 'impossible native cast: int as bool',
        'scalar-alias-cast-mismatch': 'impossible native cast: int as bool',
        'cast-move-use-after-move': 'use of explicitly moved int',
        'inference-rejects-sum-handle': "native inferred scalar cannot erase owner/nominal type of 'wrapped'",
        'cast-rejects-sum-handle': "native inferred scalar cannot erase owner/nominal type of 'wrapped'",
        'inferred-bool-arithmetic': 'arithmetic requires int, got bool',
        'cast-false-to-int': 'impossible native cast: bool as int',
        'cast-zero-to-bool': 'impossible native cast: int as bool',
        'cast-int-expression-to-bool': 'impossible native cast: int as bool',
        'cast-bool-expression-to-int': 'impossible native cast: bool as int',
        'cast-bool-call-to-int': 'impossible native cast: bool as int',
        'cast-int-call-to-bool': 'impossible native cast: int as bool',
        'cast-optional-bool-no-lowering': 'native as? requires Option<T> checked-cast lowering',
        'inferred-bool-let-int-write': 'native scalar assignment requires bool, got int',
        'inferred-int-let-bool-write': 'native scalar assignment requires int, got bool',
        'bool-cast-then-int': 'impossible native cast: bool as int',
        'cast-int-as-bool-from-variable': 'impossible native cast: int as bool',
    })
    ownership_diagnostics.update({
        'nested-identity-then-bool': 'impossible native cast: int as bool',
        'nested-bool-then-int': 'impossible native cast: bool as int',
        'identity-then-optional': 'native as? requires Option<T> checked-cast lowering',
        'identity-then-bool-arithmetic': 'arithmetic requires int, got bool',
    })
    # Two exact-byte Oreslang guest files copied into Java, LLVM, Wasm,
    # Cranelift and .NET. Git blob verification is source provenance only;
    # the compiler/linker/guest loop below supplies separate execution proof.
    native_contract = Path(__file__).resolve().parents[1] / 'contracts' / 'v1'
    for name, file_name, expected_sha, accepted in [
        ('portable-v2-positive', 'portable-checked-cast-positive-v2.ores',
         '1a80e07da1289c87d04454866883f4e06cfc8d77', True),
        ('portable-v2-negative', 'portable-checked-cast-negative-v2.ores',
         'ecc4ccea80f016243e38f351df60cfad9eb6e136', False),
    ]:
        path = native_contract / file_name
        require(path.is_file() and not path.is_symlink(), (name, 'unsafe fixture'))
        data = path.read_bytes()
        require(0 < len(data) < 1024, (name, 'unbounded fixture'))
        blob = hashlib.sha1(b'blob ' + str(len(data)).encode('ascii') + b'\0' + data).hexdigest()
        require(blob == expected_sha, (name, 'source-byte drift', blob))
        source_text = data.decode('utf-8')
        if accepted:
            positive.append((name, source_text))
        else:
            negative.append((name, source_text))
            ownership_diagnostics[name] = 'impossible native cast: int as bool'
    compared = 0
    with tempfile.TemporaryDirectory(prefix='ores-native-bool-') as tmp:
        root = Path(tmp)
        for name, code in positive:
            source = root / (name + '.ores'); source.write_bytes(code.encode('utf-8'))
            compiled = run([args.compiler, str(source)])
            require(compiled.returncode == 0, (name, compiled.stderr))
            ir = root / (name + '.ll'); ir.write_text(compiled.stdout)
            binary = root / name
            command = [args.clang, '-O1', str(ir), '-o', str(binary)]
            if args.sanitize: command[1:1] = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
            linked = run(command); require(linked.returncode == 0, (name, linked.stderr))
            executed = run([str(binary)]); require(executed.returncode == 42, (name, executed))
            if args.java_command:
                reference = run([*args.java_command, str(source)])
                require(reference.returncode == 42, ('Java', name, reference))
                compared += 1
        # A real imported helper retains bool metadata through module qualification.
        (root/'flags.ores').write_text('define module flags as pub fnc enabled():bool{return true;}end')
        imported = 'import module flags from "./flags";pub fnc main():int{const ready:bool=flags.enabled();if(ready){return 42;}fi return 0;}'
        source = root/'import.ores'; source.write_text(imported)
        compiled = run([args.compiler, str(source)]); require(compiled.returncode == 0, compiled.stderr)
        (root/'import.ll').write_text(compiled.stdout)
        linked = run([args.clang, '-O1', str(root/'import.ll'), '-o', str(root/'import')]); require(linked.returncode == 0, linked.stderr)
        require(run([str(root/'import')]).returncode == 42, 'import execution')
        if args.java_command:
            reference = run([*args.java_command, str(source)]); require(reference.returncode == 42, reference)
            compared += 1
        for name, code in negative:
            source = root / (name + '.ores'); source.write_text(code)
            result = run([args.compiler, str(source)])
            require(result.returncode == 1 and not result.stdout, (name, result))
            require(ownership_diagnostics.get(name, 'native scalar type mismatch') in result.stderr, (name, result.stderr))
    print(f'PASS native bool: {len(positive)+len(negative)+1} cases, Java differential={compared}, sanitized={args.sanitize}')


if __name__ == '__main__': main()
