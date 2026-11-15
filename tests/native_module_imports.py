#!/usr/bin/env python3
"""Compile/link/execute native module graphs; prove failure emits zero IR."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile


def run(command, **kwargs):
    return subprocess.run(command, capture_output=True, text=True, timeout=30, **kwargs)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--compiler', required=True)
    p.add_argument('--clang', required=True)
    p.add_argument('--java-command', nargs='+', help='Optional Java launcher prefix for admitted-source differential execution')
    a = p.parse_args()
    positives = [
        ('private-helper', {
            'main.ores': 'import module Math from "./math"; pub fnc main():int{return Math.add(40);}',
            'math.ores': 'define module Math as fnc delta():int{return 2;} pub fnc add(int n):int{return n+delta();} end',
        }, 42),
        ('explicit-alias', {
            'main.ores': "import module Math as M from './math.ores'; pub fnc main():int{return M.add(40);}",
            'math.ores': 'define module Math as pub fnc add(int n):int{return n+2;} end',
        }, 42),
        ('distinct-private-symbols', {
            'main.ores': 'import module A from "./a"; import module B from "./b"; pub fnc main():int{const left:int=A.answer();const right:int=B.answer();return left+right;}',
            'a.ores': 'define module A as fnc helper():int{return 20;} pub fnc answer():int{return helper();} end',
            'b.ores': 'define module B as fnc helper():int{return 22;} pub fnc answer():int{return helper();} end',
        }, 42),
        ('diamond-and-forward', {
            'main.ores': 'import module A from "./a"; import module B from "./b"; pub fnc main():int{const left:int=A.answer();const right:int=B.answer();return left+right;}',
            'a.ores': 'import module Base from "./base"; define module A as pub fnc answer():int{return Base.answer();} end',
            'b.ores': 'import module Base from "./base.ores"; define module B as pub fnc answer():int{return Base.answer();} end',
            'base.ores': 'define module Base as pub fnc answer():int{return later();} fnc later():int{return 21;} end',
        }, 42),
        ('wrapped-entry', {
            'main.ores': 'import module Math from "./math"; define module App as pub fnc main():int{return Math.add(40);} end',
            'math.ores': 'define module Math as pub fnc add(int n):int{return n+2;} end',
        }, 42),
        ('declaration-cycle', {
            'main.ores': 'import module A from "./a"; pub fnc main():int{return A.answer();}',
            'a.ores': 'import module B from "./b"; define module A as pub fnc answer():int{return B.answer();} end',
            'b.ores': 'import module A from "./a"; define module B as pub fnc answer():int{return 42;} end',
        }, 42),
        ('private-class-storage', {
            'main.ores': 'import module Boxes from "./boxes"; pub fnc main():int{return Boxes.answer();}',
            'boxes.ores': 'define module Boxes as define class Box as pub let int value=42; end fnc read(borrow Box b):int{return b.value;} pub fnc answer():int{let Box b=new Box();return read(rt borrow b);} end',
        }, 42),
        ('borrow-across-module', {
            'main.ores': 'import module Math from "./math"; pub fnc main():int{val n:int=40;return Math.add(rt borrow n);}',
            'math.ores': 'define module Math as pub fnc add(borrow int n):int{return (rt copy n)+2;} end',
        }, 42),
        ('arity-slots', {
            'main.ores': 'import module Math from "./math"; pub fnc main():int{return Math.add(40,2);}',
            'math.ores': 'define module Math as pub fnc add(int x):int{return x;} pub fnc add(int x,int y):int{return x+y;} end',
        }, 42),
        ('generic-slot', {
            'main.ores': 'import module Math from "./math"; pub fnc main():int{return Math.identity(42);}',
            'math.ores': 'define module Math as pub fnc identity<T>(T x):T{return x;} end',
        }, 42),
        ('foreign-main-not-entrypoint', {
            'main.ores': 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}',
            'lib.ores': 'define module Lib as pub fnc main():int{return 7;} pub fnc answer():int{return 42;} end',
        }, 42),
        ('reborrow-across-two-modules', {
            'main.ores': 'import module A from "./a"; pub fnc main():int{val n:int=42;return A.answer(rt borrow n);}',
            'a.ores': 'import module B from "./b"; define module A as pub fnc answer(borrow int x):int{return B.answer(rt borrow x);} end',
            'b.ores': 'define module B as pub fnc answer(borrow int x):int{return rt copy x;} end',
        }, 42),
    ]
    # Executable precursor for namespace linking: two physical provider
    # source files are compiled into one native guest. These are existing
    # module imports, NOT import namespace and NOT package/manifest admission.
    positives.append(('cross-file-scalar-43', {
        'main.ores': 'import module Charges from "./charges"; import module Balances from "./balances"; pub fnc main():int{return Charges.charge(40)+Balances.balance();}',
        'charges.ores': 'define module Charges as pub fnc charge(int cents):int{return cents+1;} end',
        'balances.ores': 'define module Balances as pub fnc balance():int{return 2;} end',
    }, 43))
    positives.append(('project-root', {
        'main.ores': 'import module Lib from "+/lib"; pub fnc main():int{return Lib.answer();}',
        '.ores.pkg.toml': 'schema_version = "1"\n',
        'lib.ores': 'define module Lib as pub fnc answer():int{return 42;} end',
    }, 42))
    positives.extend([
        ('atto-relative-source', {
            'main.atto': 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-explicit-relative-source', {
            'main.atto': 'import module Lib from "./lib.atto"; pub fnc main():int{return Lib.answer();}',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-legacy-relative-source', {
            'main.atto': 'import module Lib from "./lib.ores"; pub fnc main():int{return Lib.answer();}',
            'lib.ores': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-project-root-source', {
            'main.atto': 'import module Lib from "+/lib"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-project-root-explicit-modern-with-twin', {
            'main.atto': 'import module Lib from "+/lib.atto"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
            'lib.ores': 'define module Lib as pub fnc answer():int{return 7;} end',
        }, 42),
        ('atto-project-root-explicit-legacy-with-twin', {
            'main.atto': 'import module Lib from "+/lib.ores"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 7;} end',
            'lib.ores': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-explicit-legacy-remains-selectable', {
            'main.atto': 'import module Lib from "./lib.ores"; pub fnc main():int{return Lib.answer();}',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 7;} end',
            'lib.ores': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-diamond-linked-dependencies', {
            'main.atto': 'import module A from "./a"; import module B from "./b"; pub fnc main():int{return A.answer()+B.answer();}',
            'a.atto': 'import module Base from "./base"; define module A as pub fnc answer():int{return Base.answer();} end',
            'b.atto': 'import module Base from "./base.atto"; define module B as pub fnc answer():int{return Base.answer();} end',
            'base.atto': 'define module Base as pub fnc answer():int{return 21;} end',
        }, 42),
    ])
    # These are native .atto executable guest cases: "repo" is an actual
    # environment variable, unrelated to the independent +/ root operator.
    positives.extend([
        ('atto-env-absolute-repo', {
            'main.atto': 'import module Lib from "${repo}/lib"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-env-project-root-component', {
            'main.atto': 'import module Lib from "+/${MODULE}"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-env-multiple-variables', {
            'main.atto': 'import module Lib from "${repo}/${MODULE}"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-env-root-and-env-independent-graph', {
            'main.atto': 'import module Root from "+/root"; import module Env from "${repo}/env"; pub fnc main():int{return Root.answer()+Env.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n',
            'root.atto': 'define module Root as pub fnc answer():int{return 19;} end',
            'env.atto': 'define module Env as pub fnc answer():int{return 23;} end',
        }, 42),
        ('atto-zed-package-marker-root-guest', {
            'main.atto': 'import module Lib from "+/lib"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': '[package]\nname = "consumer"\n',
            'lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
        ('atto-manifest-configured-project-root-guest', {
            'application/main.atto': 'import module Lib from "+/src/lib"; pub fnc main():int{return Lib.answer();}',
            '.ores.pkg.toml': 'schema_version = "1"\n[project]\nroot = "application"\n',
            'application/src/lib.atto': 'define module Lib as pub fnc answer():int{return 42;} end',
        }, 42),
    ])
    library = 'define module Lib as fnc secret():int{return 41;} pub fnc answer():int{return secret()+1;} end'
    positives.append(('conditional-import-calls', {
        'main.ores': 'import module Lib from "./lib";pub fnc main():int{return Lib.ready() ? Lib.answer(21) : 0;}',
        'lib.ores': 'define module Lib as pub fnc ready():bool{return true;}pub fnc answer(int n):int{return n+21;}end',
    },42))
    positives.append(('conditional-method-calls', {
        'main.ores': 'import module Lib from "./lib";pub fnc main():int{return Lib.answer();}',
        'lib.ores': 'define module Lib as define class Flag as const bool ready=true;pub enabled():bool{return self.ready;}pub result():int{return 42;}end pub fnc answer():int{let Flag f=new Flag();return f.enabled() ? f.result() : 0;}end',
    },42))
    negatives = [
        # Import namespaces have a distinct AST identity; before checked v2
        # package-manifest integration they must emit no LLVM IR, regardless
        # of whether the quoted selector is local or a pinned dependency.
        ('namespace-local-unlinked', 'import namespace billing as pay from "+/"; pub fnc main():int{return pay.answer();}', library, 'NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED'),
        ('namespace-pinned-unlinked', 'import namespace billing as pay from "checkout"; pub fnc main():int{return pay.answer();}', library, 'NAMESPACE_RUNTIME_BACKEND_UNSUPPORTED'),
        ('namespace-traversal-selector', 'import namespace billing as pay from "../other"; pub fnc main():int{return 0;}', library, 'invalid native namespace package alias'),
        ('namespace-empty-selector', 'import namespace billing as pay from ""; pub fnc main():int{return 0;}', library, 'namespace package selector'),
        ('namespace-duplicate-local-alias', 'import namespace billing as pay from "+/"; import namespace other as pay from "checkout"; pub fnc main():int{return 0;}', library, 'duplicate imported namespace binding'),
        ('namespace-module-alias-collision', 'import module Lib as pay from "./lib"; import namespace billing as pay from "+/"; pub fnc main():int{return 0;}', library, 'duplicate imported namespace binding'),
        ('private-export', 'import module Lib from "./lib"; pub fnc main():int{return Lib.secret();}', library, 'imported function is private'),
        ('wrong-selection', 'import module Other from "./lib"; pub fnc main():int{return 0;}', library, 'does not declare selected module'),
        ('wrong-arity', 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer(2);}', library, 'wrong call arity'),
        ('unqualified-export', 'import module Lib from "./lib"; pub fnc main():int{return answer();}', library, 'unknown function'),
        ('duplicate-binding', 'import module Lib from "./lib"; import module Lib from "./lib"; pub fnc main():int{return 0;}', library, 'duplicate imported module binding'),
        ('parameter-shadow', 'import module Lib from "./lib"; fnc read(int Lib):int{return Lib;} pub fnc main():int{return 0;}', library, 'cannot be shadowed'),
        ('local-shadow', 'import module Lib from "./lib"; pub fnc main():int{val Lib:int=1;return Lib;}', library, 'cannot be shadowed'),
        ('move-after-borrow-call', 'import module Lib from "./lib"; pub fnc main():int{val n:int=1;return Lib.answer(rt take n,rt borrow n);}', 'define module Lib as pub fnc answer(int x,borrow int y):int{return x+y;} end', 'moved'),
        ('conflicting-borrows', 'import module Lib from "./lib"; pub fnc main():int{val n:int=1;return Lib.answer(rt mut borrow n,rt borrow n);}', 'define module Lib as pub fnc answer(borrow mut int x,borrow int y):int{return x+y;} end', 'borrow'),
        ('private-overload', 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer(1);}', 'define module Lib as pub fnc answer():int{return 42;} fnc answer(int n):int{return n;} end', 'private'),
        ('runtime-collision', 'import module Lib from "./lib"; fnc abort():int{return 1;} pub fnc main():int{return 0;}', library, 'reserved native runtime ABI'),
        ('init-hook', 'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}', 'define module Lib as fnc init():void{return;} pub fnc answer():int{return 42;} end', 'init hook'),
        ('missing-file', 'import module Lib from "./missing"; pub fnc main():int{return 0;}', library, 'cannot resolve native import'),
        ('std-not-faked', 'import module Lib from "std/net"; pub fnc main():int{return 0;}', library, 'native ores.std package resolution remains unsupported'),
        ('project-root-missing-manifest', 'import module Lib from "+/lib"; pub fnc main():int{return 0;}', library, 'nearest .ores.pkg.toml'),
        ('directory', 'import module Lib from "./nested"; pub fnc main():int{return 0;}', library, 'cannot resolve native import'),
        ('unterminated-path', 'import module Lib from "./lib; pub fnc main():int{return 0;}', library, 'unterminated native string'),
        ('unsupported-dependency', 'import module Lib from "./lib"; pub fnc main():int{return 0;}', 'define module Lib as pub async fnc answer():int{return 42;} end', 'native async Future<int> v1 callable must be private'),
        ('duplicate-local-arity', 'import module Lib from "./lib"; pub fnc main():int{return 0;}', 'define module Lib as pub fnc answer():int{return 42;} pub fnc answer():int{return 41;} end', 'already has arity'),
        ('foreign-invalid-utf8', 'import module Lib from "./lib"; pub fnc main():int{return 0;}', b'\xff', 'valid UTF-8'),
        ('foreign-null', 'import module Lib from "./lib"; pub fnc main():int{return 0;}', b'//\x00\n'+library.encode(), 'NUL byte'),
        ('foreign-capability-directive', 'import module Lib from "./lib"; pub fnc main():int{return 0;}', '// ores: target=native\n'+library, 'environment headers require'),
    ]
    negatives.append(('linked-conformance-not-erased',
        'import module Lib from "./lib";pub fnc main():int{return Lib.answer();}',
        'pub define contract Reply as fnc answer() => int;end define module Lib conforms Reply as pub fnc answer():int{return 42;}end',
        'native linked contract conformance requires'))
    positives.append(('named-arguments-native-only', {
        'main.ores': 'import module Math from "./math"; pub fnc main():int{return Math.add(y:2,x:40);}',
        'math.ores': 'define module Math as pub fnc add(int x,int y):int{return x+y;} end',
    }, 42))
    positives.append(('qualified-generic-native-only', {
        'main.ores': 'import module Math from "./math"; pub fnc main():int{return Math.identity<int>(42);}',
        'math.ores': 'define module Math as pub fnc identity<T>(T x):T{return x;} end',
    }, 42))
    passed = compared = 0
    with tempfile.TemporaryDirectory(prefix='ores-native-imports-') as temporary:
        root = Path(temporary)
        for name, files, expected in positives:
            directory = root/name; directory.mkdir()
            for path, source in files.items():
                (directory/path).parent.mkdir(parents=True, exist_ok=True)
                (directory/path).write_text(source)
            entry = ('application/main.atto' if 'application/main.atto' in files
                     else ('main.atto' if 'main.atto' in files else 'main.ores'))
            compiler_env = os.environ.copy()
            if name.startswith('atto-env-'):
                compiler_env.update({'repo': str(directory), 'MODULE': 'lib'})
            result = run([a.compiler, str(directory/entry)], env=compiler_env)
            if result.returncode: raise AssertionError((name, result.stderr))
            if name == 'private-helper' and 'define internal i64 @__ores_module_1.delta(' not in result.stdout:
                raise AssertionError('private imported helper leaked as an external ABI symbol')
            if name == 'cross-file-scalar-43':
                # A successful native process alone would not prove each
                # provider was compiled and called through the correct ABI.
                for signature in ('define i64 @__ores_module_1.charge(',
                                  'define i64 @__ores_module_2.balance('):
                    if signature not in result.stdout:
                        raise AssertionError(('missing physical provider IR', signature))
                for call in ('call i64 @__ores_module_1.charge(',
                             'call i64 @__ores_module_2.balance('):
                    if call not in result.stdout:
                        raise AssertionError(('missing cross-file scalar call', call))
            (directory/'guest.ll').write_text(result.stdout)
            built = run([a.clang, str(directory/'guest.ll'), '-O2', '-o', str(directory/'guest')])
            if built.returncode: raise AssertionError((name, built.stderr))
            # Prove sources are unnecessary at runtime, not loaded lazily.
            for path in files: (directory/path).rename(directory/(path+'.hidden'))
            executed = run([str(directory/'guest')], env={'PATH':'/nonexistent'})
            if executed.returncode != expected: raise AssertionError((name, executed.returncode, executed.stderr))
            for path in files: (directory/(path+'.hidden')).rename(directory/path)
            if a.java_command and entry == 'main.ores' and name not in ('named-arguments-native-only', 'qualified-generic-native-only'):
                reference = run([*a.java_command, str(directory/entry)])
                if reference.returncode != expected: raise AssertionError(('Java differential', name, reference.returncode, reference.stderr))
                compared += 1
            passed += 1
        (root / 'lib.atto').write_text('define module Lib as pub fnc answer():int{return 42;} end')
        # Negative compiler graph cases must emit no LLVM IR, never an
        # apparently successful package graph or partially linked source.
        for label, specifier, variables, expected in [
            ('missing-variable', '${ATTO_MISSING_TEST_997}/lib', {}, 'missing or empty import environment variable'),
            ('empty-variable', '${repo}/lib', {'repo': ''}, 'missing or empty import environment variable'),
            ('invalid-name', '${bad-name}/lib', {}, 'invalid ${NAME} import variable name'),
            ('nested-variable', '${repo}/lib', {'repo': '${OTHER}'}, 'invalid import environment variable value'),
            ('control-byte', '${repo}/lib', {'repo': 'bad\npath'}, 'control characters'),
            ('too-large', '${repo}/lib', {'repo': 'x' * 4097}, 'invalid import environment variable value'),
            ('root-traversal-variable', '+/${MODULE}', {'MODULE': '../outside'}, 'empty/dot/traversal'),
            ('env-outside-package-denied', '${repo}/lib', {'repo': str(root)}, 'escapes physical package root'),
            ('env-missing-manifest-denied', '${repo}/lib', {'repo': str(root / 'env-negative-env-missing-manifest-denied')}, 'absolute import requires nearest .ores.pkg.toml'),
            ('unbraced-dollar-not-alias', '$repo/lib', {'repo': str(root)}, 'native imports require'),
            ('remote-not-verified', 'https://example.test/lib.atto', {}, 'remote imports require locked'),
            ('file-uri-not-source', 'file:///tmp/lib.atto', {}, 'unsupported import URI scheme'),
            ('interpolated-java-uri-not-source', '${repo}/lib',
             {'repo': 'java:java.lang.Runtime'}, 'unsupported import URI scheme'),
        ]:
            directory = root / ('env-negative-' + label)
            directory.mkdir()
            if label != 'env-missing-manifest-denied':
                (directory / '.ores.pkg.toml').write_text('schema_version = "1"\n')
            (directory / 'main.atto').write_text(
                'import module Lib from "' + specifier + '"; pub fnc main():int{return 0;}')
            (directory / 'lib.atto').write_text(
                'define module Lib as pub fnc answer():int{return 42;} end')
            env = os.environ.copy()
            env.pop('ATTO_MISSING_TEST_997', None)
            env.update(variables)
            result = run([a.compiler, str(directory / 'main.atto')], env=env)
            if result.returncode == 0 or result.stdout or expected not in result.stderr:
                raise AssertionError((label, result.returncode, result.stdout[:150], result.stderr))
            passed += 1

        # Extensionless migration twins are ambiguous by policy. They must
        # fail before LLVM IR is emitted; explicit suffixes remain selectable.
        for name, main_source, project_root in (
            ('atto-relative-ambiguous-twins',
             'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}', False),
            ('atto-root-ambiguous-twins',
             'import module Lib from "+/lib"; pub fnc main():int{return Lib.answer();}', True),
        ):
            directory = root/name; directory.mkdir()
            (directory/'main.atto').write_text(main_source)
            (directory/'lib.atto').write_text('define module Lib as pub fnc answer():int{return 42;} end')
            (directory/'lib.ores').write_text('define module Lib as pub fnc answer():int{return 7;} end')
            if project_root:
                (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n')
            result = run([a.compiler, str(directory/'main.atto')])
            if not result.returncode or result.stdout or 'ambiguous native source import' not in result.stderr:
                raise AssertionError((name, result.returncode, result.stdout[:150], result.stderr))
            passed += 1
        # Project-root security failures must be compiler failures too, not
        # only host-resolver unit failures. Every case requires zero IR.
        directory = root/'atto-root-nested-package-boundary'; directory.mkdir()
        (directory/'main.atto').write_text(
            'import module Lib from "+/nested/lib"; pub fnc main():int{return 0;}')
        (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n')
        (directory/'nested').mkdir()
        (directory/'nested/.ores.pkg.toml').write_text('schema_version = "1"\n')
        (directory/'nested/lib.atto').write_text(
            'define module Lib as pub fnc answer():int{return 42;} end')
        result = run([a.compiler, str(directory/'main.atto')])
        if not result.returncode or result.stdout or 'nested package boundary' not in result.stderr:
            raise AssertionError(('atto nested package boundary', result.returncode,
                                  result.stdout[:150], result.stderr))
        passed += 1

        if hasattr(os, 'symlink'):
            directory = root/'atto-root-symlink-escape'; directory.mkdir()
            outside = root/'outside-root-import.atto'
            outside.write_text('define module Lib as pub fnc answer():int{return 42;} end')
            (directory/'main.atto').write_text(
                'import module Lib from "+/lib"; pub fnc main():int{return 0;}')
            (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n')
            (directory/'lib.atto').symlink_to(outside)
            result = run([a.compiler, str(directory/'main.atto')])
            if not result.returncode or result.stdout or 'escapes physical package root' not in result.stderr:
                raise AssertionError(('atto project-root symlink escape', result.returncode,
                                      result.stdout[:150], result.stderr))
            passed += 1

        if hasattr(os, 'link'):
            directory = root/'atto-hardlink-ambiguous-twins'; directory.mkdir()
            (directory/'main.atto').write_text(
                'import module Lib from "./lib"; pub fnc main():int{return Lib.answer();}')
            (directory/'lib.atto').write_text(
                'define module Lib as pub fnc answer():int{return 42;} end')
            os.link(directory/'lib.atto', directory/'lib.ores')
            result = run([a.compiler, str(directory/'main.atto')])
            if not result.returncode or result.stdout or 'ambiguous native source import' not in result.stderr:
                raise AssertionError(('atto hardlink twin ambiguity', result.returncode,
                                      result.stdout[:150], result.stderr))
            passed += 1
        # Relative ../ is allowed inside the nearest package but cannot
        # cross that package's physical root or enter a nested package.
        directory = root/'atto-relative-parent-within-package'; directory.mkdir()
        (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n')
        (directory/'app').mkdir()
        (directory/'app/main.atto').write_text(
            'import module Lib from "../lib"; pub fnc main():int{return Lib.answer();}')
        (directory/'lib.atto').write_text(
            'define module Lib as pub fnc answer():int{return 42;} end')
        result = run([a.compiler, str(directory/'app/main.atto')])
        if result.returncode:
            raise AssertionError(('relative parent within package', result.stderr))
        (directory/'guest.ll').write_text(result.stdout)
        built = run([a.clang, str(directory/'guest.ll'), '-O2', '-o', str(directory/'guest')])
        if built.returncode or run([str(directory/'guest')], env={'PATH':'/nonexistent'}).returncode != 42:
            raise AssertionError(('relative parent within package execution', built.stderr))
        passed += 1

        directory = root/'atto-relative-package-escape'; directory.mkdir()
        package = directory/'pkg'; (package/'app').mkdir(parents=True)
        (package/'.ores.pkg.toml').write_text('schema_version = "1"\n')
        (package/'app/main.atto').write_text(
            'import module Outside from "../../outside"; pub fnc main():int{return 0;}')
        (directory/'outside.atto').write_text(
            'define module Outside as pub fnc answer():int{return 42;} end')
        result = run([a.compiler, str(package/'app/main.atto')])
        if not result.returncode or result.stdout or 'escapes physical package root' not in result.stderr:
            raise AssertionError(('relative package escape', result.returncode,
                                  result.stdout[:150], result.stderr))
        passed += 1

        directory = root/'atto-relative-nested-package'; directory.mkdir()
        (directory/'app').mkdir()
        (directory/'nested').mkdir()
        (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n')
        (directory/'nested/.ores.pkg.toml').write_text('schema_version = "1"\n')
        (directory/'app/main.atto').write_text(
            'import module Lib from "../nested/lib"; pub fnc main():int{return 0;}')
        (directory/'nested/lib.atto').write_text(
            'define module Lib as pub fnc answer():int{return 42;} end')
        result = run([a.compiler, str(directory/'app/main.atto')])
        if not result.returncode or result.stdout or 'nested package boundary' not in result.stderr:
            raise AssertionError(('relative nested package crossing', result.returncode,
                                  result.stdout[:150], result.stderr))
        passed += 1

        if hasattr(os, 'symlink'):
            directory = root/'atto-relative-symlink-package-escape'; directory.mkdir()
            package = directory/'pkg'; (package/'app').mkdir(parents=True)
            (package/'.ores.pkg.toml').write_text('schema_version = "1"\n')
            outside = directory/'outside.atto'
            outside.write_text(
                'define module Outside as pub fnc answer():int{return 42;} end')
            (package/'app/main.atto').write_text(
                'import module Outside from "../outside"; pub fnc main():int{return 0;}')
            (package/'outside.atto').symlink_to(outside)
            result = run([a.compiler, str(package/'app/main.atto')])
            if not result.returncode or result.stdout or 'escapes physical package root' not in result.stderr:
                raise AssertionError(('relative symlink package escape', result.returncode,
                                      result.stdout[:150], result.stderr))
            passed += 1

            directory = root/'atto-relative-dangling-modern-legacy'; directory.mkdir()
            (directory/'main.atto').write_text(
                'import module Lib from "./lib"; pub fnc main():int{return 0;}')
            (directory/'lib.atto').symlink_to(directory/'missing.atto')
            (directory/'lib.ores').write_text(
                'define module Lib as pub fnc answer():int{return 42;} end')
            result = run([a.compiler, str(directory/'main.atto')])
            if not result.returncode or result.stdout or 'ambiguous native source import' not in result.stderr:
                raise AssertionError(('relative dangling canonical collision', result.returncode,
                                      result.stdout[:150], result.stderr))
            passed += 1

        for name, main_source, library_source, diagnostic in negatives:
            directory = root/name; directory.mkdir()
            (directory/'main.ores').write_text(main_source)
            (directory/'lib.ores').write_bytes(library_source if isinstance(library_source, bytes) else library_source.encode())
            result = run([a.compiler, str(directory/'main.ores')])
            if not result.returncode or result.stdout or diagnostic not in result.stderr:
                raise AssertionError((name, diagnostic, result.returncode, result.stdout[:150], result.stderr))
            passed += 1
        directory=root/'project-root-duplicate-schema'; directory.mkdir()
        (directory/'main.ores').write_text('import module Lib from "+/lib"; pub fnc main():int{return 0;}')
        (directory/'lib.ores').write_text(library)
        (directory/'.ores.pkg.toml').write_text('schema_version = "1"\n"schema_version" = "1"\n')
        result=run([a.compiler,str(directory/'main.ores')])
        if not result.returncode or result.stdout or 'schema_version' not in result.stderr:
            raise AssertionError(('project-root duplicate schema',result.stderr))
        passed+=1
        directory=root/'project-root-multiline'; directory.mkdir()
        (directory/'main.ores').write_text('import module Lib from "+/lib"; pub fnc main():int{return 0;}')
        (directory/'lib.ores').write_text(library)
        (directory/'.ores.pkg.toml').write_text('note = """schema_version = "1" """\nschema_version = "1"\n')
        result=run([a.compiler,str(directory/'main.ores')])
        if not result.returncode or result.stdout or 'multiline TOML' not in result.stderr:
            raise AssertionError(('project-root multiline manifest',result.stderr))
        passed+=1
        # Physical aliases are not new code-unit identities.
        for alias in ('symlink', 'hardlink'):
            directory=root/alias; directory.mkdir()
            source=directory/'main.ores'
            source.write_text('import module Self from "./alias.ores"; define module Self as pub fnc main():int{return 0;} end')
            target=directory/'alias.ores'
            if alias == 'symlink': target.symlink_to(source)
            else: os.link(source, target)
            result=run([a.compiler,str(source)])
            if not result.returncode or result.stdout or 'cannot import itself' not in result.stderr:
                raise AssertionError((alias,result.stderr))
            passed+=1
        directory=root/'fifo'; directory.mkdir()
        (directory/'main.ores').write_text('import module Lib from "./lib"; pub fnc main():int{return 0;}')
        if hasattr(os, 'mkfifo'):
            os.mkfifo(directory/'lib.ores')
            result=run([a.compiler,str(directory/'main.ores')])
            if not result.returncode or result.stdout or 'regular file' not in result.stderr:
                raise AssertionError(('FIFO source admission',result.stderr))
            passed+=1
        for limit, count, diagnostic in (('depth', 34, '32 edges'), ('units', 129, '128 source files')):
            directory=root/limit; directory.mkdir()
            if limit == 'depth':
                for i in range(count):
                    imported = f'import module M{i+1} from "./m{i+1}"; ' if i+1 < count else ''
                    (directory/f'm{i}.ores').write_text(imported+f'define module M{i} as pub fnc value():int{{return 1;}} end')
                entry=directory/'m0.ores'
            else:
                (directory/'main.ores').write_text(''.join(f'import module M{i} from "./m{i}";' for i in range(count))+ 'pub fnc main():int{return 0;}')
                for i in range(count): (directory/f'm{i}.ores').write_text(f'define module M{i} as pub fnc value():int{{return 1;}} end')
                entry=directory/'main.ores'
            result=run([a.compiler,str(entry)])
            if not result.returncode or result.stdout or diagnostic not in result.stderr:
                raise AssertionError((limit,result.stderr))
            passed+=1
        directory=root/'edge-budget'; directory.mkdir()
        (directory/'lib.ores').write_text(library)
        (directory/'main.ores').write_text(''.join(f'import module Lib as M{i} from "./lib";' for i in range(4097))+'pub fnc main():int{return 0;}')
        result=run([a.compiler,str(directory/'main.ores')])
        if not result.returncode or result.stdout or '4096 edges' not in result.stderr:
            raise AssertionError(('import edge budget',result.stderr))
        passed+=1
        directory=root/'aggregate-budget'; directory.mkdir()
        (directory/'main.ores').write_text('import module Lib from "./lib"; pub fnc main():int{return 0;}')
        (directory/'lib.ores').write_text('//'+'x'*(8*1024*1024)+'\n'+library)
        result=run([a.compiler,str(directory/'main.ores')])
        if not result.returncode or result.stdout or '8 MiB source read budget' not in result.stderr:
            raise AssertionError(('aggregate source budget',result.stderr))
        passed+=1
        directory=root/'hardlink-dedup'; directory.mkdir()
        (directory/'lib.ores').write_text('define module Lib as pub fnc value():int{return 21;} end')
        os.link(directory/'lib.ores',directory/'other.ores')
        (directory/'main.ores').write_text('import module Lib as A from "./lib"; import module Lib as B from "./other"; pub fnc main():int{const a:int=A.value();const b:int=B.value();return a+b;}')
        result=run([a.compiler,str(directory/'main.ores')])
        if result.returncode or (result.stdout.count('define i64 @__ores_module_1.value(') != 1 or result.stdout.count('define i64 @__ores_module_') != 1):
            raise AssertionError(('hardlink identity not deduplicated',result.stderr))
        passed+=1
        # Every unsupported target fails before emitting even a module header.
        for target in ('wasm32', 'thumbv7m-none-eabi'):
            result=run([a.compiler,'--target='+target,str(root/'private-helper/main.ores')])
            if not result.returncode or result.stdout or 'freestanding native imports require compiler-owned ores.core/* modules' not in result.stderr:
                raise AssertionError((target,result.stderr))
            passed+=1
    print(f'PASS native module imports: {passed} graph/ownership/execution cases, java_differential_cases={compared}, native_named_and_explicit_qualified_generic_arguments_not_claimed_as_Java_parity')

if __name__=='__main__':main()
