#!/usr/bin/env python3
"""Real normal Atto LLVM compiler -> external C ABI i64 -> native AOT.

This exercises the standard front end, not --experimental-static-ffi-i64-aot.
The CLI FFI grant is provisional and NOT a signed supervisor capability.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", required=True)
parser.add_argument("--clang", required=True)
args, unknown = parser.parse_known_args()
COMPILER = Path(args.compiler).resolve()
CLANG = Path(args.clang).resolve()
assert COMPILER.is_file() and os.access(COMPILER, os.X_OK)
assert CLANG.is_file() and os.access(CLANG, os.X_OK)

DECLARE = "extern fnc atto_add_i64(int a, int b): int using ffi;\n"
def source(lhs="20", rhs="22", effects="using ffi", declaration=DECLARE):
    return (
        declaration +
        f"pub fnc main(): int {effects} {{\n"
        f"  return atto_add_i64({lhs}, {rhs});\n"
        "}\n"
    )

def run(*command):
    return subprocess.run(command, capture_output=True, timeout=100)

class NormalCompilerStaticI64(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="atto-ffi-normal-i64-")
        cls.root = Path(cls.temp.name)
        cls.c = cls.root/"approved.c"
        cls.c.write_text(
            '#include <stdint.h>\n'
            '_Static_assert(sizeof(int64_t)==8,"i64 width");\n'
            'int64_t atto_add_i64(int64_t a,int64_t b){return a+b;}\n',
            encoding="utf-8"
        )
        cls.compiler_digest=hashlib.sha256(COMPILER.read_bytes()).hexdigest()
        print("NORMAL_FFI_COMPILER_SHA256="+cls.compiler_digest,flush=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def compile(self, name, src, grant=True, target="host"):
        path=self.root/(name+".atto")
        path.write_text(src,encoding="utf-8")
        sha=hashlib.sha256(path.read_bytes()).hexdigest()
        argv=[str(COMPILER),f"--target={target}"]
        if grant: argv.append("--ffi-grant=atto.math.i64.v1")
        result=run(*argv,str(path))
        self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(),sha,
                         "compiler changed caller source")
        return result

    def link(self, name, ir, sanitize=False, with_c=True):
        ll=self.root/(name+".ll")
        ll.write_bytes(ir)
        exe=self.root/(name+".native")
        obj=self.root/(name+".o")
        flags=["-fsanitize=address,undefined","-fno-omit-frame-pointer"] if sanitize else ["-O2"]
        if with_c:
            c=run(str(CLANG),*flags,"-c",str(self.c),"-o",str(obj))
            self.assertEqual(c.returncode,0,c.stderr.decode(errors="replace")[-1000:])
        link=run(str(CLANG),*flags,"-Wno-override-module","-x","ir",str(ll),"-x","none",
                 *([str(obj)] if with_c else []),"-o",str(exe))
        return link,exe

    def test_real_normal_compiler_guest_with_full_width_i64(self):
        examples=[
            ("answer42","20","22",42),
            ("zero","0","0",0),
            ("i64wide","9223372036854775807","-9223372036854775765",42),
            ("i64minus","9223372036854775807","-9223372036854775807",0),
            ("negative_arg","-17","59",42)
        ]
        for label,a,b,expected in examples:
            with self.subTest(label=label):
                compiled=self.compile(label,source(a,b))
                self.assertEqual(compiled.returncode,0,
                    compiled.stderr.decode(errors="replace")[-1600:])
                self.assertIn(b"declare i64 @atto_add_i64(i64, i64)",compiled.stdout)
                self.assertIn(b"call i64 @atto_add_i64",compiled.stdout)
                self.assertIn(b"define i64 @__oreslang_guest_main",compiled.stdout)
                for sanitized in (False,True):
                    suffix="_san" if sanitized else "_opt"
                    linked,exe=self.link(label+suffix,compiled.stdout,sanitize=sanitized)
                    self.assertEqual(linked.returncode,0,linked.stderr.decode(errors="replace")[-1600:])
                    executed=run(str(exe))
                    self.assertEqual(executed.returncode,expected,
                        executed.stderr.decode(errors="replace")[-1700:])
                    print("NORMAL_FFI_NATIVE_EXEC="+label+" SAN="+str(sanitized)+
                          " SHA256="+hashlib.sha256(exe.read_bytes()).hexdigest(),flush=True)

    def test_approved_foreign_linker_symbol_is_mandatory(self):
        compiled=self.compile("missing-shim",source())
        self.assertEqual(compiled.returncode,0,compiled.stderr.decode(errors="replace"))
        linked,exe=self.link("not-linked",compiled.stdout,with_c=False)
        self.assertNotEqual(linked.returncode,0)
        self.assertFalse(exe.exists())

    def test_compile_negatives_emit_no_ir(self):
        negatives=[
            ("no_grant",source(),False,"host"),
            ("wrong_symbol",source(declaration=DECLARE.replace("atto_add_i64","printf")),True,"host"),
            ("wrong_return",source(declaration=DECLARE.replace("): int","): bool")),True,"host"),
            ("wrong_param",source(declaration=DECLARE.replace("int a","bool a")),True,"host"),
            ("borrow_param",source(declaration=DECLARE.replace("int a","borrow int a")),True,"host"),
            ("foreign_without_ffi",source(declaration=DECLARE.replace(" using ffi","")),True,"host"),
            ("main_no_ffi",source(effects=""),True,"host"),
            ("nested_foreign",source().replace("return atto_add_i64(20, 22)",
                "return atto_add_i64(atto_add_i64(1,2),22)"),True,"host"),
            ("source_override",source().replace(DECLARE,
                "fnc atto_add_i64(int a,int b): int { return a+b; }\n"),True,"host"),
            ("library_path",source().replace("extern fnc","extern library \"evil.so\" fnc"),True,"host"),
            ("pointer_param",source(declaration=DECLARE.replace("int a","Pointer<int> a")),True,"host"),
            ("untrusted_actor",source().replace(DECLARE,
                "define untrusted ffi actor NativeMath as end\n"),True,"host"),
            ("wasm",source(),True,"wasm32"),
            ("overflow_literal",source("9223372036854775808","0"),True,"host"),
            ("foreign_signed_overflow",source("9223372036854775807","1"),True,"host"),
            ("foreign_signed_underflow",source("-9223372036854775807","-2"),True,"host"),
            ("unknown_effect",source(effects="using foo"),True,"host"),
            ("async_extern",source(declaration=DECLARE.replace("extern fnc", "async extern fnc")),True,"host"),
            ("async_ffi_symbol",source(declaration=DECLARE.replace("extern fnc", "extern async fnc")),True,"host"),
            ("granted_missing_extern",source(declaration=""),True,"host"),
            ("granted_unused_extern",DECLARE+"pub fnc main(): int using ffi { return 42; }\n",True,"host"),
            ("granted_without_effectful_call",DECLARE+"pub fnc main(): int { return 42; }\n",True,"host"),
            ("granted_extra_helper",DECLARE+
                "fnc helper(): int { return 2; }\n"
                "pub fnc main(): int using ffi { return atto_add_i64(20,22); }\n",True,"host"),
            ("duplicate_ffi_declaration",DECLARE+source(),True,"host"),
            ("duplicated_ffi_effect",source(declaration=DECLARE.replace("using ffi", "using ffi, ffi")),True,"host"),
            ("export_foreign",source(declaration=DECLARE.replace("extern fnc", "pub extern fnc")),True,"host")
        ]
        for label,txt,grant,target in negatives:
            with self.subTest(label=label):
                compiled=self.compile(label,txt,grant,target)
                self.assertNotEqual(compiled.returncode,0,
                    f"{label} unsafe source was accepted")
                self.assertEqual(compiled.stdout,b"",f"{label} leaked partial LLVM IR")
                print("NORMAL_FFI_NO_IR_REJECT="+label,flush=True)

    def test_ffi_grant_does_not_enable_pointer_profile(self):
        # A formerly missing guard admitted the FFI grant through the
        # early-return Pointer<int> compiler before M1 effect validation.
        # Use a genuinely supported pointer source, not invalid FFI source.
        source_text=(
            "pub fnc main(): int {\n"
            "  let mut Pointer<int> p = Pointer.alloc<int>(1);\n"
            "  p.write_at(0, 42);\n"
            "  val int first = p.read_at(0);\n"
            "  p.close();\n"
            "  return first;\n"
            "}\n"
        )
        path=self.root/"pointer-grant-bypass.atto"
        path.write_text(source_text, encoding="utf-8")
        without_grant=run(str(COMPILER),"--target=host",
            "--experimental-pointer-int-aot",str(path))
        self.assertEqual(without_grant.returncode,0,
            without_grant.stderr.decode(errors="replace"))
        self.assertIn(b"define",without_grant.stdout)
        denied=run(str(COMPILER),"--target=host",
            "--experimental-pointer-int-aot","--ffi-grant=atto.math.i64.v1",str(path))
        self.assertNotEqual(denied.returncode,0)
        self.assertEqual(denied.stdout,b"")
        self.assertIn(b"FFI",denied.stderr)
        print("NORMAL_FFI_POINTER_GRANT_BYPASS_REJECT=PASS",flush=True)

    def test_ffi_grant_is_not_ambient_authority(self):
        src="pub fnc main(): int { return 42; }\n"
        result=self.compile("ambient-ffi-grant",src,grant=True)
        self.assertNotEqual(result.returncode,0)
        self.assertEqual(result.stdout,b"")
        self.assertIn(b"grant",result.stderr)
        print("NORMAL_FFI_AMBIENT_GRANT_REJECT=PASS",flush=True)

    def test_no_foreign_symbols_can_be_forged_without_authority(self):
        # In the absence of FFI, all ordinary main source behavior remains.
        normal=self.compile("legacy-main","pub fnc main(): int {return 42;}\n",grant=False)
        self.assertEqual(normal.returncode,0,normal.stderr.decode(errors="replace"))
        self.assertNotIn(b"atto_add_i64",normal.stdout)

if __name__=="__main__":
    unittest.main(argv=[__file__,*unknown],verbosity=2)
