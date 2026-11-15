#!/usr/bin/env python3
"""Source bool stays distinct from int across loans, objects and module linking."""
import argparse
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
    negative = [
        ('bool-as-int', 'pub fnc main():int{return true;}'),
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
    compared = 0
    with tempfile.TemporaryDirectory(prefix='ores-native-bool-') as tmp:
        root = Path(tmp)
        for name, code in positive:
            source = root / (name + '.ores'); source.write_text(code)
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
