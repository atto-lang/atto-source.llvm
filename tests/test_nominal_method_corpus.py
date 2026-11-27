import unittest, subprocess, sys, tempfile, json, hashlib, shutil
from pathlib import Path
from unittest.mock import patch
import nominal_method_corpus as m

class Test(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root=Path(self.tmp.name); self.compiler=self.root/'mock'; self.clang=Path(shutil.which('clang')).resolve()
        self.compiler.write_text('#!'+sys.executable+'\nimport sys\ns=open(sys.argv[1]).read()\nif s=="valid": print("define i32 @main() { ret i32 42 }")\nelif s=="wrong": print("define i32 @main() { ret i32 7 }")\nelif s=="broken": print("not IR")\nelif s=="unsupported": print("oreslang-llvmc: NATIVE_CLASS_INHERITANCE_UNSUPPORTED",file=sys.stderr);sys.exit(1)\nelif s=="crash": sys.exit(139)\nelif s=="output": print("partial IR");print("oreslang-llvmc: obsolete method introducer",file=sys.stderr);sys.exit(1)\nelse: print("oreslang-llvmc: obsolete method introducer",file=sys.stderr);sys.exit(1)\n');self.compiler.chmod(0o700)
    def c(self,source='valid',id='class.test',positive=True):
        return dict(id=id,source=source,expect='compile_and_run',expected_return_int=42) if positive else dict(id=id,source=source,expect='syntax_reject',diagnostic_family='OBSOLETE_METHOD_INTRODUCER')
    def check(self,case,status): self.assertEqual(m.verify_case(case,self.compiler,self.clang,False)['status'],status)
    def test_native_driver(self): self.check(self.c(),'PASS_GUEST')
    def test_wrong_return(self): self.check(self.c('wrong'),'FAIL')
    def test_failed_link(self): self.check(self.c('broken'),'FAIL')
    def test_negative(self): self.check(self.c('reject',positive=False),'PASS_DIAGNOSTIC')
    def test_crash(self): self.check(self.c('crash',positive=False),'FAIL')
    def test_partial_ir(self): self.check(self.c('output',positive=False),'FAIL')
    def test_unexpected_accept(self): self.check(self.c('valid',positive=False),'FAIL')
    def test_unsupported(self): self.check(self.c('unsupported','interface.test'),'UNSUPPORTED_PROFILE')
    def test_wrong_scope(self): self.check(self.c('unsupported'),'FAIL')
    def test_wrong_family(self): self.check(dict(self.c('reject',positive=False),diagnostic_family='MISSING_INSTANCE_OBLIGATION'),'FAIL')
    def test_manifest_pin(self):
        p=self.root/'manifest'; p.write_text('{}')
        with self.assertRaisesRegex(ValueError,'wrong corpus'): m.verify(p,self.compiler,m.sha256(self.compiler),self.clang,m.sha256(self.clang))
    def test_toolchain_pin(self):
        p=self.root/'manifest'; data=b'{"cases":[]}'; p.write_bytes(data)
        with patch.object(m,'CORPUS_BLOB',hashlib.sha1(b'blob %d\0'%len(data)+data).hexdigest()):
            with self.assertRaisesRegex(ValueError,'toolchain digest mismatch'): m.verify(p,self.compiler,'0'*64,self.clang,m.sha256(self.clang))
    def test_exit_status(self):
        for n,counts,want in [(120,{'PASS_GUEST':120},0),(120,{'PASS_GUEST':119,'UNSUPPORTED_PROFILE':1},2),(120,{'FAIL':1},1),(119,{'PASS_GUEST':119},1)]:
            self.assertEqual(m.exit_status({'cases':[{}]*n,'counts':counts}),want)
if __name__=='__main__':unittest.main()
