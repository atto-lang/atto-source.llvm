#!/usr/bin/env python3
"""Trusted-main, filter-free checkout verifier. Run outside the PR's code."""
import os, re, stat, subprocess, sys
from pathlib import Path
class Denied(Exception): pass
OID=re.compile(rb"(?:[0-9a-f]{40}|[0-9a-f]{64})\Z")
def env(): return dict(os.environ,GIT_NO_REPLACE_OBJECTS="1",GIT_CONFIG_NOSYSTEM="1",GIT_CONFIG_GLOBAL=os.devnull,GIT_OPTIONAL_LOCKS="0")
def git(*a):
    p=subprocess.run(["git","-c","core.fsmonitor=false","-c","core.untrackedCache=false","-c","core.hooksPath=/dev/null",*a],env=env(),stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    if p.returncode: raise Denied("Git metadata query failed")
    return p.stdout
def parts(p):
    if not p or p.startswith(b"/") or b"\\" in p or any(x in (b"",b".",b"..",b".git") for x in p.split(b"/")): raise Denied("unsafe path")
    return p.split(b"/")
def tree():
    d={}
    for x in git("ls-tree","-r","-z","--full-tree","HEAD").split(b"\0"):
        if not x: continue
        m,sep,p=x.partition(b"\t"); fields=m.split(b" ")
        if not sep or len(fields)!=3: raise Denied("bad tree")
        mode,kind,oid=fields; parts(p)
        if mode not in (b"100644",b"100755") or kind!=b"blob" or not OID.fullmatch(oid) or p in d: raise Denied("unsupported tree entry")
        d[p]=(mode,oid)
    return d
def index():
    d={}
    for x in git("ls-files","--stage","-z").split(b"\0"):
        if not x: continue
        m,sep,p=x.partition(b"\t"); fields=m.split(b" ")
        if not sep or len(fields)!=3: raise Denied("bad index")
        mode,oid,stage=fields; parts(p)
        if mode not in (b"100644",b"100755") or stage!=b"0" or not OID.fullmatch(oid) or p in d: raise Denied("staged or unsupported index entry")
        d[p]=(mode,oid)
    return d
def untracked():
    if git("ls-files","--others","-z").strip(b"\0"): raise Denied("untracked/ignored files")
def read(s,n):
    x=s.read(n)
    if len(x)!=n: raise Denied("short blob read")
    return x
def compare(root,d):
    p=subprocess.Popen(["git","-c","core.fsmonitor=false","-c","core.hooksPath=/dev/null","cat-file","--batch"],env=env(),stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
    try:
        for path,(mode,oid) in sorted(d.items()):
            ds=[]; fd=root
            try:
                ps=parts(path)
                for part in ps[:-1]:
                    fd=os.open(part,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW,dir_fd=fd);ds.append(fd)
                ffd=os.open(ps[-1],os.O_RDONLY|os.O_NOFOLLOW|os.O_NONBLOCK,dir_fd=fd)
                with os.fdopen(ffd,"rb",buffering=0) as f:
                    before=os.fstat(f.fileno())
                    if not stat.S_ISREG(before.st_mode) or before.st_nlink!=1: raise Denied("unsafe file type or hardlink")
                    if bool(before.st_mode&0o111)!=(mode==b"100755"): raise Denied("mode drift")
                    p.stdin.write(oid+b"\n");p.stdin.flush()
                    hdr=p.stdout.readline().rstrip(b"\n").split(b" ")
                    if len(hdr)!=3 or hdr[:2]!=[oid,b"blob"] or not hdr[2].isdigit(): raise Denied("invalid Git object")
                    size=int(hdr[2])
                    if before.st_size!=size: raise Denied("size drift")
                    while size:
                        n=min(size,1048576)
                        if f.read(n)!=read(p.stdout,n): raise Denied("blob content drift")
                        size-=n
                    if read(p.stdout,1)!=b"\n": raise Denied("invalid object boundary")
                    after=os.fstat(f.fileno())
                    for attr in ("st_dev","st_ino","st_size","st_mode","st_ctime_ns","st_mtime_ns"):
                        if getattr(before,attr)!=getattr(after,attr): raise Denied("concurrent file mutation")
            except OSError as e: raise Denied("unsafe checkout path") from e
            finally:
                for x in reversed(ds): os.close(x)
    finally:
        p.stdin.close();p.stdout.close()
        try: p.wait(timeout=5)
        except subprocess.TimeoutExpired: p.kill();p.wait()
        if p.returncode: raise Denied("Git object reader failed")
def main():
    if len(sys.argv)!=2 or not OID.fullmatch(os.fsencode(sys.argv[1])): raise Denied("invalid expected SHA")
    expected=os.fsencode(sys.argv[1])
    if git("rev-parse","--verify","HEAD").strip()!=expected: raise Denied("wrong HEAD")
    root=Path(os.fsdecode(git("rev-parse","--show-toplevel").strip())).resolve()
    if root!=Path.cwd().resolve(): raise Denied("wrong checkout directory")
    d=tree()
    if d!=index(): raise Denied("HEAD/index differ")
    untracked()
    fd=os.open(root,os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)
    try: compare(fd,d)
    finally: os.close(fd)
    if git("rev-parse","--verify","HEAD").strip()!=expected or d!=index(): raise Denied("HEAD/index changed")
    untracked()
    print("FILTER_FREE_EXACT_HEAD_INDEX_WORKTREE=PASS files="+str(len(d)))
if __name__=="__main__":
    try: main()
    except (Denied,OSError,ValueError,subprocess.SubprocessError) as e:
        print("FILTER_FREE_EXACT_HEAD_INDEX_WORKTREE=DENIED: "+str(e),file=sys.stderr);sys.exit(1)
