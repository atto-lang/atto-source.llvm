#!/usr/bin/env python3
"""Source-free adversarial tests: create only synthetic local Git repositories."""
import os,sys,subprocess,tempfile,unittest
from pathlib import Path
CHECK=Path(__file__).with_name("verify_llvm_exact_checkout.py")
def cmd(c,*a,check=True):
    return subprocess.run(a,cwd=c,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,check=check)
class VerifyTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory();self.addCleanup(self.temp.cleanup)
        self.dir=Path(self.temp.name)
        cmd(self.dir,"git","init","-q")
        cmd(self.dir,"git","config","user.name","Fixture")
        cmd(self.dir,"git","config","user.email","fixture@example.invalid")
        (self.dir/"normal.txt").write_text("original\n")
        (self.dir/"bin").mkdir();p=self.dir/"bin"/"cmd"
        p.write_text("#!/bin/sh\nexit 0\n");p.chmod(0o755)
        self.commit()
    def commit(self):
        cmd(self.dir,"git","add",".");cmd(self.dir,"git","commit","-qm","fixture")
        self.sha=cmd(self.dir,"git","rev-parse","HEAD").stdout.strip()
    def check(self,allowed=False,sha=None):
        r=cmd(self.dir,sys.executable,str(CHECK),sha or self.sha,check=False)
        self.assertEqual(r.returncode==0,allowed,r.stdout+r.stderr)
    def test_clean(self): self.check(True)
    def test_wrong_sha(self): self.check(sha="b"*40)
    def test_dirty_bytes(self):
        (self.dir/"normal.txt").write_text("modification\n");self.check()
    def test_staged(self):
        (self.dir/"normal.txt").write_text("staged\n")
        cmd(self.dir,"git","add","normal.txt");self.check()
    def test_untracked(self):
        (self.dir/"extra").write_text("untracked");self.check()
    def test_ignored(self):
        (self.dir/".git"/"info"/"exclude").write_text("ignored\n")
        (self.dir/"ignored").write_text("ignored");self.check()
    def test_symlink(self):
        p=self.dir/"normal.txt";p.unlink();p.symlink_to("/etc/hosts");self.check()
    def test_parent_symlink(self):
        p=self.dir/"bin";p.rename(self.dir/"old_bin")
        p.symlink_to("old_bin",target_is_directory=True);self.check()
    def test_fifo(self):
        p=self.dir/"normal.txt";p.unlink();os.mkfifo(p);self.check()
    def test_mode(self):
        (self.dir/"normal.txt").chmod(0o755);self.check()
    def test_hardlink(self):
        (self.dir/"another").hardlink_to(self.dir/"normal.txt");self.check()
    def test_committed_symlink(self):
        (self.dir/"link").symlink_to("normal.txt");self.commit();self.check()
    def test_clean_filter(self):
        (self.dir/".gitattributes").write_text("normal.txt filter=tripwire\n");self.commit()
        marker=self.dir/"FILTER_EXECUTED"
        cmd(self.dir,"git","config","filter.tripwire.clean",f'sh -c "touch {marker}; cat"')
        self.check(True);self.assertFalse(marker.exists())
        cmd(self.dir,"git","diff","--quiet",check=False)
        self.assertTrue(marker.exists(),"negative control: Git diff should execute filter")
if __name__=="__main__": unittest.main(verbosity=2)
