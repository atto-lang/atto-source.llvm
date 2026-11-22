#!/usr/bin/env python3
"""Actual Atto .atto -> real LLVM18 IR -> statically linked C ABI -> native binary.

Prototype only: rejects all but the one fixed foreign i64 symbol and narrow
test grammar. This does NOT implement actor spawn/supervision, foreign callbacks,
dynamic loading, general FFI or full language source lowering.
"""
import argparse
import hashlib
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

cli = argparse.ArgumentParser()
cli.add_argument("--compiler", required=True)
cli.add_argument("--clang", required=True)
args, others = cli.parse_known_args()
COMPILER = Path(args.compiler).resolve()
CLANG = Path(args.clang).resolve()
assert COMPILER.is_file() and os.access(COMPILER, os.X_OK), "REAL_LLVM_COMPILER_REQUIRED"
assert CLANG.is_file() and os.access(CLANG, os.X_OK), "REAL_CLANG_REQUIRED"
FLAG = "--experimental-static-ffi-i64-aot"
GRANT = "--ffi-grant=atto.math.i64.v1"

def source(lhs=20, rhs=22):
    return (
        "define ffi actor NativeMath as\n"
        "  extern suspends fnc add_i64(int a, int b): int;\n"
        "end\n"
        "pub suspends fnc main(): int using ffi, suspend {\n"
        f"  return await NativeMath.add_i64({lhs}, {rhs});\n"
        "}\n"
    )

def execute(*cmd, **kw):
    return subprocess.run(cmd, timeout=120, capture_output=True, **kw)

class StaticFfiNativeCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="atto-native-ffi-i64-")
        cls.root = Path(cls.tmp.name)
        cls.compiler_sha = hashlib.sha256(COMPILER.read_bytes()).hexdigest()
        print("FFI_REAL_COMPILER_SHA256="+cls.compiler_sha,flush=True)
        cls.shim = cls.root / "approved.c"
        cls.shim.write_text(
            "#include <stdint.h>\n"
            "_Static_assert(sizeof(int64_t)==8, \"i64 must be 8 bytes\");\n"
            "int64_t atto_add_i64(int64_t a, int64_t b) { return a+b; }\n",
            encoding="utf8")

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def compile(self, name, src, grant=True, enabled=True):
        path = self.root/(name+".atto")
        path.write_text(src,encoding="utf8")
        digest = hashlib.sha256(path.read_bytes()).hexdigest()
        cmd = [str(COMPILER), "--target=host"]
        if enabled: cmd.append(FLAG)
        if grant: cmd.append(GRANT)
        result = execute(*cmd,str(path))
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),digest,
                         "compiler may not modify checked input")
        return result

    def link_native(self, name, ir, sanitizer=False, provide_shim=True):
        ll = self.root/(name+".ll")
        ll.write_bytes(ir)
        exe = self.root/(name+".native")
        obj = self.root/(name+".o")
        flags = ["-fsanitize=address,undefined","-fno-omit-frame-pointer"] if sanitizer else ["-O2"]
        if provide_shim:
            build = execute(str(CLANG),*flags,"-c",str(self.shim),"-o",str(obj))
            self.assertEqual(build.returncode,0,build.stderr.decode(errors="replace")[-1400:])
        cmd = [str(CLANG),*flags,"-Wno-override-module","-x","ir",str(ll),"-x","none"]
        if provide_shim: cmd.append(str(obj))
        cmd += ["-o",str(exe)]
        result = execute(*cmd)
        return result,exe

    def test_real_native_foreign_downcalls(self):
        for name,lhs,rhs in [("answer42",20,22),("tiny",1,2),
                             ("zero",0,0),("max",155,100),("nested",40,2)]:
            with self.subTest(name=name):
                result = self.compile(name,source(lhs,rhs))
                self.assertEqual(result.returncode,0,result.stderr.decode(errors="replace"))
                ir = result.stdout
                self.assertIn(b"declare i64 @atto_add_i64(i64, i64)",ir)
                self.assertIn(b"call i64 @atto_add_i64",ir)
                self.assertIn(b"define i32 @main()",ir)
                for sanitized in [False,True]:
                    link,exe = self.link_native(name+("-asan" if sanitized else ""),ir,sanitizer=sanitized)
                    self.assertEqual(link.returncode,0,link.stderr.decode(errors="replace")[-1800:])
                    actual=execute(str(exe))
                    self.assertEqual(actual.returncode,lhs+rhs,
                        f"real compiled C foreign call failed {name}: {actual.stderr.decode(errors='replace')[-1600:]}")
                    print("FFI_REAL_NATIVE_GUEST_PASS="+name+
                          " ASAN_UBSAN="+str(sanitized)+
                          " binary_sha256="+hashlib.sha256(exe.read_bytes()).hexdigest(),flush=True)

    def test_without_approved_linker_symbol_refuses_native_artifact(self):
        result=self.compile("link-guard",source())
        self.assertEqual(result.returncode,0)
        linked,exe=self.link_native("without-approved-shim",result.stdout,provide_shim=False)
        self.assertNotEqual(linked.returncode,0,"unresolved FFI symbol must fail linking")
        self.assertFalse(exe.exists(),"no native artifact on failed foreign linkage")

    def test_wrong_source_or_capability_rejects_without_ir(self):
        negatives=[
            ("no-grant",source(),False,True),
            ("no-mode",source(),True,False),
            ("wrong-return",source().replace("add_i64(int a, int b): int",
                                            "add_i64(int a, int b): float"),True,True),
            ("wrong-callee",source().replace("NativeMath.add_i64","NativeMath.open"),True,True),
            ("foreign-path",source().replace("extern suspends fnc",
                                            "extern library \"evil.so\" suspends fnc"),True,True),
            ("foreign-borrow",source().replace("int a","Pointer<int> a"),True,True),
            ("ffi-untrusted",source().replace("define ffi actor",
                                            "define untrusted ffi actor"),True,True),
            ("callback",source().replace("int b","fnc(int) => int b"),True,True),
            ("overflow",source(255,1),True,True),
            ("negative",source(-1,2),True,True),
            ("dangling",source()+"fnc extra(): int { return 0; }",True,True),
            ("foreign-alias",source().replace("add_i64(int a, int b)", "printf(int a, int b)"),True,True),
        ]
        for name,text,grant,enabled in negatives:
            with self.subTest(name=name):
                result=self.compile(name,text,grant=grant,enabled=enabled)
                self.assertNotEqual(result.returncode,0,name)
                self.assertEqual(result.stdout,b"",name+" leaked LLVM output")
                self.assertNotIn(b"declare i64 @",result.stdout)
                print("FFI_NO_IR_NEGATIVE_PASS="+name,flush=True)

if __name__ == "__main__":
    unittest.main(argv=[__file__,*others],verbosity=2)
