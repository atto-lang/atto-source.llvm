#!/usr/bin/env python3
"""Real instance-method/qualified-class lowering, with receiver ownership gates."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

COUNTER = '''pub define class Counter as
  let int value=40;
  pub read():int{return self.value;}
  pub add(mut self)(int n):void{self.value=self.value+n;return;}
end
'''

def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, timeout=45, **kwargs)

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--compiler',required=True)
    p.add_argument('--clang',required=True)
    p.add_argument('--sanitize',action='store_true')
    p.add_argument('--java-command',nargs='+')
    a=p.parse_args()
    positives=[
        ('private-field-read', {'main.ores':COUNTER+'pub fnc main():int{let Counter c=new Counter();return c.read()+2;}'}),
        ('mutable-receiver', {'main.ores':COUNTER+'pub fnc main():int{let mut Counter c=new Counter();c.add(2);return c.read();}'}),
        ('private-self-call', {'main.ores':'''define class Box as let int value=42;
          hidden():int{return self.value;}
          pub read():int{return self.hidden();} end
          pub fnc main():int{let Box b=new Box();return b.read();}'''}),
        ('explicit-arity-slots', {'main.ores':'''define class Box as let int value=40;
          pub read():int{return self.value;}
          pub read(int n):int{return self.value+n;} end
          pub fnc main():int{let Box b=new Box();return b.read(2);}'''}),
        ('constructor-before-methods', {'main.ores':'''pub define class Box as let int value=start;
          pub constructor(int start){return;}
          pub read()->int{return self.value;} end
          pub fnc main():int{let Box b=new Box(42);return b.read();}'''}),
        ('shared-reborrow', {'main.ores':COUNTER+'''fnc read(borrow Counter c):int{return c.read()+2;}
          pub fnc main():int{let Counter c=new Counter();return read(rt borrow c);}'''}),
        ('exclusive-reborrow', {'main.ores':COUNTER+'''fnc bump(borrow mut Counter c):void{c.add(2);return;}
          pub fnc main():int{let mut Counter c=new Counter();bump(rt mut borrow c);return c.read();}'''}),
        ('same-type-private-local', {'main.ores':'''define class Box as let int value=21;
          pub read():int{let Box other=new Box();return self.value+other.value;} end
          pub fnc main():int{let Box b=new Box();return b.read();}'''}),
        ('method-main-is-not-entry', {'main.ores':'''define class Box as let int value=42;
          pub main():int{return self.value;} end
          pub fnc main():int{let Box b=new Box();return b.main();}'''}),
        ('imported-public-class', {'main.ores':'''import module Lib from "./lib";
          pub fnc main():int{let mut Lib.Counter c=new Lib.Counter();c.add(2);return c.read();}''',
          'lib.ores':'define module Lib as '+COUNTER+' end'}),
        ('imported-nominal-identities', {'main.ores':'''import module A from "./a";import module B from "./b";
          pub fnc main():int{let A.Box a=new A.Box();let B.Box b=new B.Box();const x:int=a.read();const y:int=b.read();return x+y;}''',
          'a.ores':'define module A as pub define class Box as let int value=20;pub read():int{return self.value;} end end',
          'b.ores':'define module B as pub define class Box as let int value=22;pub read():int{return self.value;} end end'}),
        ('imported-data-only-class', {'main.ores':'''import module Lib from "./lib";
          pub fnc main():int{let Lib.Box b=new Lib.Box();return b.value;}''',
          'lib.ores':'define module Lib as pub define class Box as pub let int value=42;end end'}),
    ]
    positives.append(('module-class-helper', {
        'main.ores':'import module Lib from "./lib";pub fnc main():int{return Lib.answer();}',
        'lib.ores':'define module Lib as '+COUNTER+'pub fnc answer():int{let mut Counter c=new Counter();c.add(2);return c.read();}end',
    }))
    positives.append(('ordinary-send-is-not-actor-send', {
        'main.ores':'define class Sender as let int value=40;pub send(int n):int{return self.value+n;}end pub fnc main():int{let Sender s=new Sender();return s.send(2);}',
    }))
    # The receiver is declaration metadata, not a caller argument. These
    # run as native LLVM guests (with the normal ASan/UBSan variant).
    positives.extend([
        ('inline-read-self', {'main.ores':'''define class Box as let int value=40;
          pub read(self, int n):int{return self.value+n;}end
          pub fnc main():int{let Box b=new Box();return b.read(2);}'''}),
        ('zero-value-inline-read-self', {'main.ores':'''define class Box as let int value=40;
          pub read(self):int{return self.value+2;}end
          pub fnc main():int{let Box b=new Box();return b.read();}'''}),
        ('inline-mut-self', {'main.ores':'''define class Box as let int value=40;
          pub add(mut self, int n):void{self.value=self.value+n;return;}
          pub read():int{return self.value;}end
          pub fnc main():int{let mut Box b=new Box();b.add(2);return b.read();}'''}),
        ('zero-value-split-mut-self', {'main.ores':'''define class Box as let int value=40;
          pub read(mut self)():int{return self.value+2;}end
          pub fnc main():int{let mut Box b=new Box();return b.read();}'''}),
        ('distinct-names-same-arity', {'main.ores':'''define class Box as let int value=40;
          pub x(int n):int{return n;}
          pub y(mut self, int n):int{return self.value+n;}end
          pub fnc main():int{let mut Box b=new Box();return b.x(1)+b.y(1);}'''}),
    ])
    java_unsupported={'imported-public-class','imported-nominal-identities','imported-data-only-class'}
    negatives=[
        ('public-constructor-private-class', 'define class Box as let int value=42;pub constructor(){return;}end pub fnc main():int{return 0;}',{},'public constructor requires a public class'),
        ('field-method-collision', 'define class Box as let int read=42;pub read():int{return 42;}end pub fnc main():int{return 0;}',{},'field/method name collision'),
        ('split-arrow-not-an-operator', 'define class Box as let int value=42;pub read() - > int{return self.value;}end pub fnc main():int{return 0;}',{},"expected '->'"),
        ('private-field', COUNTER+'pub fnc main():int{let Counter c=new Counter();return c.value;}',{},'private class field'),
        ('private-method', '''define class Box as let int value=42;hidden():int{return self.value;}end
          pub fnc main():int{let Box b=new Box();return b.hidden();}''',{},'private instance method'),
        ('readonly-write', '''define class Box as let int value=42;
          pub write():void{self.value=2;return;}end pub fnc main():int{return 0;}''',{},'shared class borrow'),
        ('immutable-owner-mut-method', COUNTER+'pub fnc main():int{let Counter c=new Counter();c.add(2);return c.read();}',{},'immut'),
        ('shared-receiver-escalation', COUNTER+'''fnc bad(borrow Counter c):int{c.add(2);return 0;}
          pub fnc main():int{return 0;}''',{},'mutably reborrow shared'),
        ('duplicate-method-arity', '''define class Box as let int value=42;
          pub read(int n):int{return n;} pub read(borrow int n):int{return n;}end
          pub fnc main():int{return 0;}''',{},'duplicate native method'),
        # Equal receiver-erased slots always conflict, regardless of mutability,
        # declaration order, parameter type, or inline/split self spelling.
        ('implicit-then-mut-zero', '''define class Foo as
          x():void{return;} x(mut self)():void{return;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/0'"),
        ('mut-then-implicit-zero', '''define class Foo as
          x(mut self)():void{return;} x():void{return;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/0'"),
        ('implicit-then-inline-mut-one', '''define class Foo as
          x(int n):int{return n;} x(mut self, int n):int{return n;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/1'"),
        ('inline-mut-then-implicit-one', '''define class Foo as
          x(mut self, int n):int{return n;} x(int n):int{return n;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/1'"),
        ('inline-read-then-split-mut-one', '''define class Foo as
          x(self, int n):int{return n;} x(mut self)(int n):int{return n;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/1'"),
        ('read-and-mut-zero', '''define class Foo as
          x(self):int{return 1;} x(mut self):int{return 2;}end
          pub fnc main():int{return 0;}''',{},"duplicate native method name/arity 'x/0'"),
        ('wrong-method-arity', COUNTER+'pub fnc main():int{let Counter c=new Counter();return c.read(1,2);}',{},'explicit call arity'),
        ('method-field-take', '''define class Box as let int value=42;
          pub take(mut self)():int{return rt take self.value;}end pub fnc main():int{return 0;}''',{},'borrow'),
        ('method-final-write', '''define class Box as final int value=42;
          pub write(mut self)():void{self.value=2;return;}end pub fnc main():int{return 0;}''',{},'final class field'),
        ('method-receiver-alias', '''define class Box as let int value=42;
          pub read():int{return self.value;}
          pub combine(mut self)(borrow Box other):int{return other.read();}end
          pub fnc main():int{let mut Box b=new Box();return b.combine(rt borrow b);}''',{},'active native borrow'),
        ('foreign-private-class', '''import module Lib from "./lib";
          pub fnc main():int{let Lib.Box b=new Lib.Box();return b.read();}''',
          {'lib.ores':'define module Lib as define class Box as let int value=42;pub read():int{return self.value;}end end'},'private imported class'),
        ('foreign-private-constructor', '''import module Lib from "./lib";
          pub fnc main():int{let Lib.Box b=new Lib.Box(42);return b.read();}''',
          {'lib.ores':'define module Lib as pub define class Box as let int value=start;constructor(int start){return;}pub read():int{return self.value;}end end'},'private imported constructor'),
        ('foreign-nominal-mismatch', '''import module A from "./a";import module B from "./b";
          fnc read(borrow A.Box b):int{return b.read();}
          pub fnc main():int{let B.Box b=new B.Box();return read(rt borrow b);}''',
          {'a.ores':'define module A as pub define class Box as let int value=42;pub read():int{return self.value;}end end',
           'b.ores':'define module B as pub define class Box as let int value=42;pub read():int{return self.value;}end end'},'borrowed argument type mismatch'),
        ('byvalue-class-method-argument', '''define class Box as let int value=42;
          pub read(Box other):int{return 1;}end pub fnc main():int{return 0;}''',{},'by-value class parameter'),
        ('static-not-reinterpreted', '''define class Box as let int value=42;
          pub static fnc read():int{return 1;}end pub fnc main():int{return 0;}''',{},'expected'),
        # Source-only ancestry is NOT native class lowering: these must reject
        # before producing LLVM IR, regardless of whether main uses the class.
        ('class-inheritance-vtable-unimplemented', '''define class Base as
          pub value():int{return 1;}end
          define class Child extends Base as
          pub value():int{return 2;}end
          pub fnc main():int{return 0;}''',{},'NATIVE_CLASS_INHERITANCE_UNSUPPORTED'),
        ('class-interface-witness-unimplemented', '''define class Child implements Named as
          pub name():int{return 42;}end
          pub fnc main():int{return 0;}''',{},'NATIVE_CLASS_INHERITANCE_UNSUPPORTED'),
        ('class-multiple-inheritance-unimplemented', '''define class Left as end
          define class Right as end
          define class Diamond extends Left, Right as end
          pub fnc main():int{return 0;}''',{},'NATIVE_CLASS_INHERITANCE_UNSUPPORTED'),
        ('escaped-bound-method', COUNTER+'pub fnc main():int{let Counter c=new Counter();return c.read;}',{},'unknown class field'),
    ]
    passed=compared=0
    with tempfile.TemporaryDirectory(prefix='ores-native-method-') as temporary:
        root=Path(temporary)
        for name,files in positives:
            directory=root/name;directory.mkdir()
            for path,source in files.items():(directory/path).write_text(source)
            source=directory/'main.ores'
            result=run([a.compiler,str(source)])
            if result.returncode:raise AssertionError((name,result.stderr))
            (directory/'main.ll').write_text(result.stdout)
            command=[a.clang,'-O1',str(directory/'main.ll'),'-o',str(directory/'main')]
            if a.sanitize:command[1:1]=['-fsanitize=address,undefined','-fno-omit-frame-pointer']
            linked=run(command)
            if linked.returncode:raise AssertionError((name,linked.stderr))
            ran=run([str(directory/'main')])
            if ran.returncode!=42:raise AssertionError((name,ran.returncode,ran.stderr))
            if a.java_command and name not in java_unsupported:
                reference=run([*a.java_command,str(source)])
                if reference.returncode!=42:raise AssertionError(('Java differential',name,reference.returncode,reference.stderr))
                compared+=1
            passed+=1
        for name,source,dependencies,diagnostic in negatives:
            directory=root/name;directory.mkdir()
            (directory/'main.ores').write_text(source)
            for path,text in dependencies.items():(directory/path).write_text(text)
            result=run([a.compiler,str(directory/'main.ores')])
            # A parser crash, kill, timeout, or warning-only failure is not
            # compiler-owned rejection evidence. Match the native frontend's
            # documented diagnostic prefix and exact nonzero error exit.
            if (result.returncode != 1 or result.stdout or
                    not result.stderr.startswith('oreslang-llvmc: ') or
                    diagnostic not in result.stderr):
                raise AssertionError((name,diagnostic,result.returncode,result.stdout[:80],result.stderr))
            passed+=1
    print(f'PASS native class methods: {passed} cases, Java differential={compared}, sanitized={a.sanitize}; qualified imported construction is native-only (current Java runtime rejects it)')

if __name__=='__main__':main()
