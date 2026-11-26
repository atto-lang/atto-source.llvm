// Restricted trusted-host API for one immutable, standalone source unit.
// Compiler and toolchain identities must be independently attested by the
// host; no ambient imports, caller-supplied output or guest signing API.
// This is opt-in, not the default CLI or a full package cache.
use super::{ArtifactStore,authority::Authority,receipt};
use crate::{compiler_cache_key as key,sha256::hex_digest};
use std::path::Path;

pub(crate) struct NativeSession {
    store:ArtifactStore,authority:Authority,scope:String,compiler:String,toolchain:String,
}
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
            configuration:&hex_digest(b"standalone-trusted-host-v1"),sources:&hex_digest(source.as_bytes()),
            bindings:&hex_digest(b"no external inputs"),generated:None,dependencies:&[]}).map_err(str::to_owned)?;
        let mut out=[0;32];for(i,b)in out.iter_mut().enumerate(){*b=u8::from_str_radix(&digest[i*2..i*2+2],16).map_err(|e|e.to_string())?;}Ok(out)
    }
    pub(crate) fn build(&self,source:&str,current_pin:&[u8;32],master:[u8;32],cold:bool)->Result<Build,String>{
        let key=self.cache_key(source)?;
        let lease=self.authority.lease(&self.scope,current_pin,master).map_err(|e|e.to_string())?;
        let secret=lease.secret();
        if !cold {if let Some(bytes)=self.store.load(&key,&secret).map_err(|e|e.to_string())?{return Ok(Build{bytes,reused:true});}}
        let bytes=crate::compile_ir(source).map(String::into_bytes)?;
        let envelope=receipt::seal(&key,&bytes,&secret).map_err(str::to_owned)?;
        self.store.publish(&key,&envelope,&secret).map_err(|e|e.to_string())?;
        Ok(Build{bytes,reused:false})
    }
}
