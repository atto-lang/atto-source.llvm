//! Linux-only candidate bounded disk store. Fixed 16 direct-mapped slots
//! (4 MiB payload maximum + headers and one pending entry). The configured
//! directory and ancestors must be protected by the host; same-UID malicious
//! processes and arbitrary files written externally are outside the quota
//! guarantee. No guest execution, source verification or key management.
use crate::compiler_artifact_receipt as receipt;
use std::ffi::CString;
use std::fs::File;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::os::raw::{c_char, c_int};
use std::os::unix::fs::MetadataExt;
use std::path::{Path,Component};

const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_CREAT: c_int = 0o100;
const O_EXCL: c_int = 0o200;
const O_DIRECTORY: c_int = 0o200000;
const O_NOFOLLOW: c_int = 0o400000;
const O_CLOEXEC: c_int = 0o2000000;
const O_PATH: c_int = 0o10000000;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const PENDING: &str = ".pending";
extern "C" {
    fn openat(dirfd: c_int, pathname: *const c_char, flags: c_int, ...) -> c_int;
    fn unlinkat(dirfd: c_int, pathname: *const c_char, flags: c_int) -> c_int;
    fn renameat(oldfd: c_int, old: *const c_char, newfd: c_int, new: *const c_char) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn geteuid() -> u32;
}
fn invalid(detail: &str) -> io::Error { io::Error::new(io::ErrorKind::InvalidData,detail) }
fn slot(key: &[u8;32]) -> String { format!("slot-{:x}",key[0] >> 4) }
fn open_child(root:&File,name:&str,flags:c_int,mode:c_int)->io::Result<File>{
    let name=CString::new(name).map_err(|_|invalid("invalid slot name"))?;
    // SAFETY: NUL-terminated fixed name, valid directory descriptor, mode
    // supplied for O_CREAT; successful descriptor transferred to File.
    let fd=unsafe{openat(root.as_raw_fd(),name.as_ptr(),flags,mode)};
    if fd<0 {return Err(io::Error::last_os_error());}
    Ok(unsafe{File::from_raw_fd(fd)})
}
fn remove_pending(root:&File)->io::Result<()> {
    let name=CString::new(PENDING).unwrap();
    // SAFETY: pinned directory and fixed name; unlink never follows symlinks.
    if unsafe{unlinkat(root.as_raw_fd(),name.as_ptr(),0)}==0{return Ok(());}
    let e=io::Error::last_os_error();if e.kind()==io::ErrorKind::NotFound{Ok(())}else{Err(e)}
}

// Traverse each component without following symlinks. The host must protect
// the root's ancestors; only the final inode is checked for owner-only mode.
struct LockedRoot { directory: File }
impl LockedRoot {
    fn directory(&self)->&File { &self.directory }
    fn open(path:&Path)->io::Result<Self>{
        let absolute=if path.is_absolute(){path.to_path_buf()}else{std::env::current_dir()?.join(path)};
        let mut directory=File::open("/")?;
        for part in absolute.components(){
            match part {
                Component::RootDir|Component::CurDir=>{},
                Component::Normal(name)=>{
                    let name=name.to_str().ok_or_else(||invalid("non-UTF8 root component"))?;
                    directory=open_child(&directory,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0)?;
                },
                _=>return Err(invalid("root traversal denied")),
            }
        }
        Ok(Self{directory})
    }
}
pub struct ArtifactStore { root:LockedRoot }
impl ArtifactStore {
    pub fn open(path:&Path)->io::Result<Self>{
        let root=LockedRoot::open(path).map_err(|_|invalid("invalid cache root"))?;
        let meta=root.directory().metadata()?;
        // SAFETY: geteuid has no arguments or memory effects.
        if meta.uid()!=unsafe{geteuid()}||meta.mode()&0o077!=0{return Err(invalid("cache directory must be owner-only"));}
        Ok(Self{root})
    }
    pub fn load(&self,key:&[u8;32],secret:&[u8;32])->io::Result<Option<Vec<u8>>>{
        let root=self.root.directory();
        // O_PATH never opens a device or blocks on FIFO; stat the actual
        // descriptor before opening a regular inode through trusted procfs.
        let pinned=match open_child(root,&slot(key),O_PATH|O_NOFOLLOW|O_CLOEXEC,0){
            Ok(file)=>file,Err(e) if e.kind()==io::ErrorKind::NotFound=>return Ok(None),Err(e)=>return Err(e)
        };
        let meta=pinned.metadata()?;
        if !meta.is_file()||meta.nlink()!=1||meta.uid()!=unsafe{geteuid()}||meta.mode()&0o077!=0||meta.len()>(receipt::HEADER+receipt::MAX_ARTIFACT) as u64{
            return Err(invalid("not a bounded regular cache file"));
        }
        // Host must provide a trusted Linux /proc mount. Reopen the pinned
        // inode, not its replaceable name, and verify identity on the new FD.
        let file=File::open(format!("/proc/self/fd/{}",pinned.as_raw_fd()))?;
        let opened=file.metadata()?;
        if !opened.is_file()||opened.dev()!=meta.dev()||opened.ino()!=meta.ino()||opened.uid()!=unsafe{geteuid()}||opened.mode()&0o077!=0{
            return Err(invalid("cache descriptor identity changed"));
        }
        let mut bytes=Vec::new();
        file.take((receipt::HEADER+receipt::MAX_ARTIFACT+1) as u64).read_to_end(&mut bytes)?;
        if bytes.len()<receipt::HEADER||bytes.len()>receipt::HEADER+receipt::MAX_ARTIFACT{
            return Err(invalid("cache file size"));
        }
        // Only a structurally valid, authenticated occupant of this slot
        // may count as a collision. A corrupted key field is a diagnostic.
        let actual: [u8;32]=bytes[8..40].try_into().map_err(|_|invalid("cache key length"))?;
        if slot(&actual)!=slot(key){return Err(invalid("cache slot mismatch"));}
        let payload=receipt::verify(&actual,&bytes,secret).map_err(invalid)?;
        if &actual!=key{return Ok(None);}
        Ok(Some(payload))
    }
    pub fn publish(&self,key:&[u8;32],envelope:&[u8],secret:&[u8;32])->io::Result<()>{
        // Verify before touching an existing entry; no untrusted partial write.
        receipt::verify(key,envelope,secret).map_err(invalid)?;
        let root=self.root.directory();
        // Independent open-file description for each writer, even threads
        // sharing this store. Advisory lock serializes cooperating publishers.
        let lock=open_child(root,".",O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0)?;
        if unsafe{flock(lock.as_raw_fd(),LOCK_EX|LOCK_NB)}!=0{return Err(io::Error::last_os_error());}
        // Under the lock, stale pending from an interrupted writer is removed.
        remove_pending(root)?;
        let mut file=open_child(root,PENDING,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0o600)?;
        let result=(|| {
            file.write_all(envelope)?;file.sync_all()?;
            let from=CString::new(PENDING).unwrap();let to=CString::new(slot(key)).unwrap();
            // SAFETY: both fixed names are relative to the same pinned root.
            if unsafe{renameat(root.as_raw_fd(),from.as_ptr(),root.as_raw_fd(),to.as_ptr())}!=0{
                return Err(io::Error::last_os_error());
            }
            // A directory fsync failure after rename is an indeterminate
            // durability result, not a claim that the old slot survived.
            // Any reader still authenticates the complete returned bytes.
            root.sync_all()
        })();
        if result.is_err(){let _=remove_pending(root);}
        result
        // Dropping lock closes the independent FD and releases flock.
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    use crate::compiler_artifact_receipt::tests::{KEY,SECRET,PAYLOAD};
    use std::{fs,os::unix::fs::{DirBuilderExt,symlink},process::Command,sync::atomic::{AtomicUsize,Ordering}};
    static NEXT:AtomicUsize=AtomicUsize::new(0);
    struct Temp(std::path::PathBuf);
    impl Temp {
        fn new()->Self{
            let path=std::env::temp_dir().join(format!("atto-cache-test-{}-{}",std::process::id(),NEXT.fetch_add(1,Ordering::Relaxed)));
            fs::DirBuilder::new().mode(0o700).create(&path).unwrap();Self(path)
        }
    }
    impl Drop for Temp{fn drop(&mut self){let _=fs::remove_dir_all(&self.0);}}
    #[test]
    fn disk_roundtrip_restart_tampering_and_eviction(){
        let t=Temp::new();let store=ArtifactStore::open(&t.0).unwrap();
        let data=receipt::seal(&KEY,PAYLOAD,&SECRET).unwrap();
        assert!(store.load(&KEY,&SECRET).unwrap().is_none());
        store.publish(&KEY,&data,&SECRET).unwrap();
        let status=Command::new(std::env::current_exe().unwrap()).args(["--exact","compiler_artifact_store::tests::child_read","--nocapture"])
            .env("ATTO_CACHE_TEST_CHILD_ROOT",&t.0).status().unwrap();
        assert!(status.success());
        let path=t.0.join(slot(&KEY));
        let mut forged=data.clone();forged[112]^=1;
        let hash=crate::sha256::sha256(&forged[112..]);forged[48..80].copy_from_slice(&hash);
        assert!(store.publish(&KEY,&forged,&SECRET).is_err());
        assert_eq!(store.load(&KEY,&SECRET).unwrap().unwrap(),PAYLOAD);
        fs::write(&path,&forged).unwrap();assert!(store.load(&KEY,&SECRET).is_err());
        let mut altered_key=data.clone();altered_key[39]^=1;
        fs::write(&path,&altered_key).unwrap();assert!(store.load(&KEY,&SECRET).is_err());
        let mut wrong_slot=data.clone();wrong_slot[8]^=0x80;
        fs::write(&path,&wrong_slot).unwrap();assert!(store.load(&KEY,&SECRET).is_err());
        fs::write(t.0.join(PENDING),b"partial").unwrap();
        store.publish(&KEY,&data,&SECRET).unwrap();
        assert!(!t.0.join(PENDING).exists());
        assert_eq!(store.load(&KEY,&SECRET).unwrap().unwrap(),PAYLOAD);
        let mut other=KEY;other[31]^=1;
        store.publish(&other,&receipt::seal(&other,b"other",&SECRET).unwrap(),&SECRET).unwrap();
        assert!(store.load(&KEY,&SECRET).unwrap().is_none());
        assert_eq!(store.load(&other,&SECRET).unwrap().unwrap(),b"other");
        println!("CACHE_STORE_RUST|second-process|forgery|interrupted-writer|eviction");
    }
    #[test]
    fn child_read(){
        if let Some(root)=std::env::var_os("ATTO_CACHE_TEST_CHILD_ROOT"){
            let store=ArtifactStore::open(Path::new(&root)).unwrap();
            assert_eq!(store.load(&KEY,&SECRET).unwrap().unwrap(),PAYLOAD);
            println!("CACHE_STORE_CHILD|verified-payload");
        }
    }
    #[test]
    fn rejects_symlink_fifo_directory_hardlink_and_oversized(){
        let t=Temp::new();let store=ArtifactStore::open(&t.0).unwrap();
        let path=t.0.join(slot(&KEY));let outside=t.0.join("fixture");
        fs::write(&outside,receipt::seal(&KEY,PAYLOAD,&SECRET).unwrap()).unwrap();
        symlink(&outside,&path).unwrap();assert!(store.load(&KEY,&SECRET).is_err());fs::remove_file(&path).unwrap();
        fs::hard_link(&outside,&path).unwrap();assert!(store.load(&KEY,&SECRET).is_err());fs::remove_file(&path).unwrap();
        fs::create_dir(&path).unwrap();assert!(store.load(&KEY,&SECRET).is_err());fs::remove_dir(&path).unwrap();
        assert!(Command::new("mkfifo").arg(&path).status().unwrap().success());
        assert!(store.load(&KEY,&SECRET).is_err());fs::remove_file(&path).unwrap();
        File::create(&path).unwrap().set_len((receipt::HEADER+receipt::MAX_ARTIFACT+1) as u64).unwrap();
        assert!(store.load(&KEY,&SECRET).is_err());
        println!("CACHE_STORE_RUST|symlink|hardlink|directory|fifo|oversized-rejected");
    }
    #[test]
    fn authenticated_slot_requires_owner_only_permissions(){
        use std::os::unix::fs::PermissionsExt;
        let t=Temp::new();let store=ArtifactStore::open(&t.0).unwrap();
        let data=receipt::seal(&KEY,PAYLOAD,&SECRET).unwrap();
        store.publish(&KEY,&data,&SECRET).unwrap();
        let path=t.0.join(slot(&KEY));
        for mode in [0o644,0o660,0o604] {
            fs::set_permissions(&path,fs::Permissions::from_mode(mode)).unwrap();
            assert!(store.load(&KEY,&SECRET).is_err(),"admitted mode {mode:o}");
        }
        fs::set_permissions(&path,fs::Permissions::from_mode(0o600)).unwrap();
        assert_eq!(store.load(&KEY,&SECRET).unwrap().unwrap(),PAYLOAD);
        println!("CACHE_STORE_RUST|authenticated-but-public-mode-rejected");
    }
    #[test]
    fn root_swap_and_writer_lock(){
        let t=Temp::new();let real=t.0.join("real");
        fs::DirBuilder::new().mode(0o700).create(&real).unwrap();
        let store=ArtifactStore::open(&real).unwrap();let data=receipt::seal(&KEY,PAYLOAD,&SECRET).unwrap();
        store.publish(&KEY,&data,&SECRET).unwrap();
        let retired=t.0.join("retired");fs::rename(&real,&retired).unwrap();
        symlink(&retired,&real).unwrap();assert!(ArtifactStore::open(&real).is_err());
        assert_eq!(store.load(&KEY,&SECRET).unwrap().unwrap(),PAYLOAD);
        let lock=open_child(store.root.directory(),".",O_RDONLY|O_DIRECTORY|O_CLOEXEC,0).unwrap();
        assert_eq!(unsafe{flock(lock.as_raw_fd(),LOCK_EX|LOCK_NB)},0);
        assert_eq!(store.publish(&KEY,&data,&SECRET).unwrap_err().kind(),io::ErrorKind::WouldBlock);
        drop(lock);store.publish(&KEY,&data,&SECRET).unwrap();
    }


    // Fixture-only integration: compile a closed scalar source and reuse the
    // exact authenticated native output after process restart. No production
    // publisher or authority provider; fixture signing key never leaves tests.
    const A:&str="pub fnc main(): int { return 41+1; }";
    const B:&str="pub fnc main(): int { return 41+2; }";
    const BAD:&str="pub fnc main(): int { return missing; }";
    fn fixture_key(source:&str)->[u8;32]{
        use crate::{sha256::hex_digest,compiler_cache_key as k};
        // This test has no external source inputs. The executable is the
        // compiler/emitter closure for this bounded fixture, not a production
        // toolchain attestation; the native linker runs after artifact reuse.
        static COMPILER:std::sync::OnceLock<String>=std::sync::OnceLock::new();
        let compiler=COMPILER.get_or_init(||{
            let mut bytes=Vec::new();
            File::open("/proc/self/exe").unwrap().take(256*1024*1024+1).read_to_end(&mut bytes).unwrap();
            assert!(bytes.len()<=256*1024*1024);
            hex_digest(&bytes)
        });
        let digest=k::encode(&k::Input{stage:"llvm-ir",owner:"@test/native-cache",unit_kind:"source-unit",unit:"main.atto",
            compiler,toolchain:&hex_digest(b"fixture: embedded emitter"),target:&hex_digest(b"linux-x86_64-host"),
            configuration:&hex_digest(b"fixture: closed scalar default profile"),sources:&hex_digest(source.as_bytes()),
            bindings:&hex_digest(b"no external imports"),generated:None,dependencies:&[]}).unwrap();
        let mut key=[0u8;32];for(i,b)in key.iter_mut().enumerate(){*b=u8::from_str_radix(&digest[i*2..i*2+2],16).unwrap();}key
    }
    fn compile_fixture(source:&str)->Result<Vec<u8>,String>{
        assert!([A,B,BAD].contains(&source));
        crate::compile_ir(source).map(String::into_bytes)
    }
    fn fixture_build(store:&ArtifactStore,source:&str,cold:bool,compile:impl FnOnce(&str)->Result<Vec<u8>,String>)->Result<(Vec<u8>,bool),String>{
        let key=fixture_key(source);
        if !cold {if let Some(bytes)=store.load(&key,&SECRET).map_err(|e|e.to_string())?{return Ok((bytes,true));}}
        let bytes=compile(source)?;
        let envelope=receipt::seal(&key,&bytes,&SECRET).map_err(str::to_owned)?;
        store.publish(&key,&envelope,&SECRET).map_err(|e|e.to_string())?;
        Ok((bytes,false))
    }
    fn execute_native(root:&Path,bytes:&[u8],expected:i32){
        let input=root.join("guest.ll");fs::write(&input,bytes).unwrap();let exe=root.join("native-guest");
        assert!(Command::new("clang").arg(&input).arg("-o").arg(&exe).status().unwrap().success());
        assert_eq!(Command::new(&exe).status().unwrap().code(),Some(expected));
    }
    #[test]
    fn persistent_native_guest(){
        let t=Temp::new();let store=ArtifactStore::open(&t.0).unwrap();
        let (cold,hit)=fixture_build(&store,A,true,compile_fixture).unwrap();assert!(!hit);
        assert_eq!(cold,compile_fixture(A).unwrap());execute_native(&t.0,&cold,42);
        let status=Command::new(std::env::current_exe().unwrap()).args(["--exact","compiler_artifact_store::tests::persistent_native_guest_child","--nocapture"])
            .env("ATTO_NATIVE_CACHE_CHILD",&t.0).status().unwrap();assert!(status.success());
        let (edited,hit)=fixture_build(&store,B,false,compile_fixture).unwrap();assert!(!hit);
        assert_eq!(edited,compile_fixture(B).unwrap());execute_native(&t.0,&edited,43);
        let before=fs::read(t.0.join(slot(&fixture_key(B)))).unwrap();
        assert!(fixture_build(&store,BAD,true,compile_fixture).is_err());
        assert_eq!(before,fs::read(t.0.join(slot(&fixture_key(B)))).unwrap());
        assert_eq!(fixture_build(&store,B,false,|_|panic!("unexpected recompile")).unwrap(),(edited,true));
        let path=t.0.join(slot(&fixture_key(B)));let mut corrupt=before;corrupt[80]^=1;fs::write(path,corrupt).unwrap();
        assert!(fixture_build(&store,B,false,|_|panic!("corruption must not compile fallback")).is_err());
        println!("PERSISTENT_NATIVE_LLVM|cold=42|warm-child=42|edited=43|failed-edit-rollback|corruption-denied");
    }
    #[test]
    fn persistent_native_guest_child(){
        if let Some(root)=std::env::var_os("ATTO_NATIVE_CACHE_CHILD"){
            let path=Path::new(&root);let store=ArtifactStore::open(path).unwrap();
            let(bytes,hit)=fixture_build(&store,A,false,|_|panic!("child must not compile")).unwrap();
            assert!(hit);execute_native(path,&bytes,42);
            println!("PERSISTENT_NATIVE_CHILD|cache-hit-no-compile|guest=42");
        }
    }
}
