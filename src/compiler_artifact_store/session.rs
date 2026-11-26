// Restricted trusted-host API for one immutable, standalone source unit.
// Compiler and toolchain identities must be independently attested by the
// host; no ambient imports, caller-supplied output or guest signing API.
// This is opt-in, not the default CLI or a full package cache.
use super::{ArtifactStore,authority::Authority,receipt};
use crate::{compiler_cache_key as key,sha256::hex_digest};
use std::path::Path;
use std::collections::{BTreeMap,BTreeSet};

pub(crate) struct NativeSession {
    store:ArtifactStore,authority:Authority,scope:String,compiler:String,toolchain:String,
}
#[derive(Debug)]
pub(crate) struct Build {pub(crate) bytes:Vec<u8>,pub(crate) reused:bool}
impl NativeSession {
    pub(crate) fn open(cache:&Path,authority:&Path,scope:&str,compiler:&str,toolchain:&str)->Result<Self,String>{
        let store=ArtifactStore::open(cache).map_err(|e|e.to_string())?;
        let authority=Authority::open(authority).map_err(|e|e.to_string())?;
        authority.distinct_from(&store).map_err(|e|e.to_string())?;
        let session=Self{store,authority,scope:scope.into(),compiler:compiler.into(),toolchain:toolchain.into()};
        session.cache_key("")?;
        Ok(session)
    }
    fn cache_key(&self,source:&str)->Result<[u8;32],String>{
        if source.len()>65536{return Err("E_CACHE_SOURCE: source budget".into());}
        // Deliberately conservative: no import/include/package constructs,
        // including occurrences in inert text. Ordinary compiler is not
        // changed. This admission profile has no external source closure.
        if source.split(|c:char|!c.is_ascii_alphanumeric()&&c!='_').any(|s|matches!(s,"import"|"include"|"package"|"namespace"|"extern"|"external"|"jvm"|"dotnet"|"use")){
            return Err("E_CACHE_SOURCE: standalone profile only".into());
        }
        let digest=key::encode(&key::Input{stage:"llvm-ir",owner:&self.scope,unit_kind:"source-unit",unit:"main.atto",
            compiler:&self.compiler,toolchain:&self.toolchain,target:&hex_digest(b"linux-x86_64"),
            configuration:&hex_digest(b"freestanding-core-trusted-host-v1"),sources:&hex_digest(source.as_bytes()),
            bindings:&hex_digest(b"no external inputs"),generated:None,dependencies:&[]}).map_err(str::to_owned)?;
        let mut out=[0;32];for(i,b)in out.iter_mut().enumerate(){*b=u8::from_str_radix(&digest[i*2..i*2+2],16).map_err(|e|e.to_string())?;}Ok(out)
    }
    pub(crate) fn build(&self,source:&str,current_pin:&[u8;32],master:[u8;32],cold:bool)->Result<Build,String>{
        let key=self.cache_key(source)?;
        let lease=self.authority.lease(&self.scope,current_pin,master).map_err(|e|e.to_string())?;
        let secret=lease.secret();
        if !cold {if let Some(bytes)=self.store.load(&key,&secret).map_err(|e|e.to_string())?{return Ok(Build{bytes,reused:true});}}
        let bytes=crate::compile_ir_with_profile(source,crate::LibraryProfile::FreestandingCore).map(String::into_bytes)?;
        let envelope=receipt::seal(&key,&bytes,&secret).map_err(str::to_owned)?;
        self.store.publish(&key,&envelope,&secret).map_err(|e|e.to_string())?;
        Ok(Build{bytes,reused:false})
    }
}

// Canonical, closed host inventory profile. Each member explicitly declares
// its own namespace and public integer function; no inferred local exports.
// This is not the general package manifest or final linked guest cache.
pub(crate) struct ProviderInput<'a>{
    pub path:&'a str,pub namespace:&'a str,pub symbol:&'a str,pub arity:usize,pub source:&'a str,
}
fn provider_ident(s:&str)->bool{
    !s.is_empty()&&s.len()<=128&&s.bytes().enumerate().all(|(i,c)|
        c.is_ascii_alphabetic()||c==b'_'||(i>0&&c.is_ascii_digit()))
}
fn inventory_pins(inputs:&[ProviderInput<'_>])->Result<(String,String),String>{
    if inputs.is_empty()||inputs.len()>128{return Err("E_NAMESPACE_INVENTORY: member budget".into());}
    let mut sorted=BTreeMap::new();let mut symbols=BTreeSet::new();
    for p in inputs{
        if p.path.len()>256||!p.path.ends_with(".atto")||
            !p.path[..p.path.len()-5].split('/').all(provider_ident)||
            !provider_ident(p.namespace)||!provider_ident(p.symbol)||p.arity>4||p.source.len()>4096||
            sorted.insert(p.path,p).is_some()||!symbols.insert((p.namespace,p.symbol)){
            return Err("E_NAMESPACE_INVENTORY: invalid or duplicate member".into());
        }
    }
    let mut manifest=String::from("atto.llvm.provider-inventory.v1\n");
    let mut sources=String::from("atto.llvm.provider-sources.v1\n");
    for p in sorted.values(){
        manifest.push_str(&format!("{}\n{}\n{}\n{}\n",p.path,p.namespace,p.symbol,p.arity));
        sources.push_str(&format!("{}\n{}\n",p.path,hex_digest(p.source.as_bytes())));
    }
    Ok((hex_digest(manifest.as_bytes()),hex_digest(sources.as_bytes())))
}
impl NativeSession{
    // Both expected pins are independently obtained from current trusted
    // host authority, not accepted from a guest or inferred from input bytes.
    // Acquire the existing epoch lease before admission and keep it through
    // lookup/emission/publication. Every member is freshly frontend-admitted.
    pub(crate) fn build_namespace_provider(&self,inputs:&[ProviderInput<'_>],
        expected_manifest:&str,expected_sources:&str,namespace:&str,symbol:&str,arity:usize,
        current_pin:&[u8;32],master:[u8;32],cold:bool)->Result<Build,String>{
        let lease=self.authority.lease(&self.scope,current_pin,master).map_err(|e|e.to_string())?;
        let (manifest,sources)=inventory_pins(inputs)?;
        if manifest!=expected_manifest||sources!=expected_sources{
            return Err("E_NAMESPACE_PIN: inventory mismatch".into());
        }
        let mut selected=None;
        for p in inputs{
            let program=crate::namespace_provider::admit_arity(p.source,p.namespace,p.symbol,p.arity)?;
            if p.namespace==namespace&&p.symbol==symbol{
                if arity!=p.arity{return Err("E_NAMESPACE_ARITY: incompatible selected call".into());}
                selected=Some((p,program));
            }
        }
        let (p,program)=selected.ok_or("E_NAMESPACE_SYMBOL: selected provider absent")?;
        // Full selected exact source, identity, ABI and current manifest
        // domain. Unrelated authenticated member bodies cannot affect the
        // closed frontend program, which has no imports or ambient inputs.
        let binding=hex_digest(format!("{}\n{}\n{}\n{}",p.path,namespace,symbol,arity).as_bytes());
        let digest=key::encode(&key::Input{stage:"llvm-provider-ir-bound-input-v2",owner:&self.scope,
            unit_kind:"source-unit",unit:p.path,compiler:&self.compiler,toolchain:&self.toolchain,
            target:&hex_digest(b"linux-x86_64"),configuration:&hex_digest(b"closed-provider-checked-i64-trap-v1"),
            sources:&hex_digest(p.source.as_bytes()),bindings:&binding,generated:None,
            dependencies:&[key::Dependency{identity:"provider-inventory",digest:&manifest}]}).map_err(str::to_owned)?;
        let mut key=[0;32];for(i,b)in key.iter_mut().enumerate(){*b=u8::from_str_radix(&digest[i*2..i*2+2],16).map_err(|e|e.to_string())?;}
        let secret=lease.secret();
        if !cold {if let Some(bytes)=self.store.load(&key,&secret).map_err(|e|e.to_string())?{return Ok(Build{bytes,reused:true});}}
        let bytes=crate::emit_admitted_ir(&program,Some((symbol,arity)))?.into_bytes();
        self.store.publish(&key,&receipt::seal(&key,&bytes,&secret).map_err(str::to_owned)?,&secret).map_err(|e|e.to_string())?;
        Ok(Build{bytes,reused:false})
    }
}
#[cfg(test)]
mod provider_tests{
    use super::*;
    use crate::{compiler_cache_ledger as ledger,sha256::sha256};
    use std::{fs,os::unix::fs::DirBuilderExt,process::Command};
    const MASTER:[u8;32]=[7;32];
    const SCOPE:&str="tenant/provider";
    const A:&str="namespace billing;\npub fnc get(int x, int y): int { return x * y + 2; }\n";
    const B:&str="namespace billing;\npub fnc get(int x, int y): int { return x * y + 3; }\n";
    const OTHER:&str="namespace identity;\npub fnc other(): int { return 900; }\n";
    const OTHER_EDIT:&str="namespace identity;\npub fnc other(): int { return 901; }\n";
    fn inputs<'a>(source:&'a str,other:&'a str)->[ProviderInput<'a>;2]{
        [ProviderInput{path:"src/billing.atto",namespace:"billing",symbol:"get",arity:2,source},
        ProviderInput{path:"src/identity.atto",namespace:"identity",symbol:"other",arity:0,source:other}]
    }
    fn pin()->[u8;32]{sha256(&ledger::record(SCOPE,1,MASTER).unwrap())}
    fn open(root:&Path)->NativeSession{
        NativeSession::open(&root.join("cache"),&root.join("authority"),SCOPE,
            &hex_digest(b"fixture compiler"),&hex_digest(b"fixture toolchain")).unwrap()
    }
    fn build(s:&NativeSession,source:&str,other:&str,cold:bool)->Build{
        let i=inputs(source,other);let(m,p)=inventory_pins(&i).unwrap();
        s.build_namespace_provider(&i,&m,&p,"billing","get",2,&pin(),MASTER,cold).unwrap()
    }
    fn execute(root:&Path,bytes:&[u8],expected:i32){
        let ir=root.join("provider.ll");fs::write(&ir,bytes).unwrap();
        let obj=root.join("provider.o");
        assert!(Command::new("clang").args(["-x","ir","-O2","-c"]).arg(&ir).arg("-o").arg(&obj).status().unwrap().success());
        let host=root.join("host.c");fs::write(&host,"#include <stdint.h>\nextern int64_t atto_namespace_provider(int64_t,int64_t);\nint main(void){return (int)atto_namespace_provider(10,4);}\n").unwrap();
        let exe=root.join("guest");
        assert!(Command::new("cc").args([&host,&obj]).arg("-Wl,-z,noexecstack").arg("-o").arg(&exe).status().unwrap().success());
        assert_eq!(Command::new(exe).status().unwrap().code(),Some(expected));
    }
    #[test]
    fn persistent_bound_provider(){
        let root=std::env::temp_dir().join(format!("atto-llvm-bound-provider-{}",std::process::id()));
        fs::DirBuilder::new().mode(0o700).create(&root).unwrap();
        struct Cleanup(std::path::PathBuf);impl Drop for Cleanup{fn drop(&mut self){let _=fs::remove_dir_all(&self.0);}}
        let _cleanup=Cleanup(root.clone());
        for name in ["cache","authority"]{fs::DirBuilder::new().mode(0o700).create(root.join(name)).unwrap();}
        Authority::open(&root.join("authority")).unwrap().provision(SCOPE,1,MASTER,&pin()).unwrap();
        let s=open(&root);
        let cold=build(&s,A,OTHER,true);assert!(!cold.reused);
        assert_eq!(cold.bytes,crate::namespace_provider::compile_arity(A,"billing","get",2).unwrap().into_bytes());
        execute(&root,&cold.bytes,42);
        let warm=build(&s,A,OTHER_EDIT,false);assert!(warm.reused);assert_eq!(cold.bytes,warm.bytes);
        let i=inputs(A,OTHER);let(m,p)=inventory_pins(&i).unwrap();let edited=inputs(A,OTHER_EDIT);
        assert!(s.build_namespace_provider(&edited,&m,&p,"billing","get",2,&pin(),MASTER,false).unwrap_err().contains("E_NAMESPACE_PIN"));
        let bad=inputs(A,"namespace identity;\npub fnc other(): int { return missing; }\n");
        let(bm,bp)=inventory_pins(&bad).unwrap();
        assert!(s.build_namespace_provider(&bad,&bm,&bp,"billing","get",2,&pin(),MASTER,false).is_err());
        assert!(s.build_namespace_provider(&i,&m,&p,"billing","get",1,&pin(),MASTER,false).is_err());
        assert!(s.build_namespace_provider(&i,&m,&p,"billing","get",2,&[0;32],MASTER,false).is_err());
        assert!(build(&s,A,OTHER_EDIT,false).reused);
        drop(s);
        let child=Command::new(std::env::current_exe().unwrap())
            .args(["--exact","compiler_artifact_store::session::provider_tests::persistent_bound_provider_child","--nocapture"])
            .env("ATTO_BOUND_PROVIDER_CHILD",&root).status().unwrap();
        assert!(child.success());
        let s=open(&root);let edit=build(&s,B,OTHER_EDIT,false);assert!(!edit.reused);
        assert_eq!(edit.bytes,crate::namespace_provider::compile_arity(B,"billing","get",2).unwrap().into_bytes());
        execute(&root,&edit.bytes,43);
        println!("LLVM_BOUND_PROVIDER|guest=42/43|unrelated-edit-hit|restart-hit|current-pin-and-arity-denied|clean-equivalent");
    }
    #[test]
    fn persistent_bound_provider_child(){
        if let Some(root)=std::env::var_os("ATTO_BOUND_PROVIDER_CHILD"){
            let root=Path::new(&root);let b=build(&open(root),A,OTHER_EDIT,false);assert!(b.reused);
            execute(root,&b.bytes,42);println!("LLVM_BOUND_PROVIDER_CHILD|hit|guest=42");
        }
    }
}
