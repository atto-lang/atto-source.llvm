//! Offline verifier for a *verifier-issued* Ed25519-signed LLVM challenge.
//! No signer keys or signing implementation live in the guest producer.
//! The signature gates one execution; the resulting artifact is UNSIGNED.
use std::{collections::BTreeMap,env,fs,process::{Command,Stdio},
    time::{SystemTime,UNIX_EPOCH}};
#[derive(Clone,Debug,PartialEq)]enum V{S(String),N(u64)}
fn json(b:&[u8])->Result<BTreeMap<String,V>,String>{
    if b.is_empty()||b.len()>4096||!b.is_ascii(){return Err("invalid challenge bytes".into())}
    fn ws(b:&[u8],i:&mut usize){while *i<b.len()&&b" \r\n\t".contains(&b[*i]){*i+=1}}
    fn quoted(b:&[u8],i:&mut usize)->Result<String,String>{
        if b.get(*i)!=Some(&b'"'){return Err("expected JSON string".into())}
        *i+=1;let start=*i;
        while *i<b.len()&&b[*i]!=b'"'{
            if b[*i]<32||b[*i]>126||b[*i]==b'\\'{return Err("noncanonical JSON string".into())}
            *i+=1;
        }
        if *i>=b.len(){return Err("unclosed JSON string".into())}
        let s=std::str::from_utf8(&b[start..*i]).unwrap().to_owned();*i+=1;Ok(s)
    }
    let mut i=0;ws(b,&mut i);
    if b.get(i)!=Some(&b'{'){return Err("JSON must be object".into())}
    i+=1;let mut o=BTreeMap::new();
    loop{
        ws(b,&mut i);if b.get(i)==Some(&b'}'){i+=1;break}
        let k=quoted(b,&mut i)?;ws(b,&mut i);
        if b.get(i)!=Some(&b':'){return Err("missing JSON colon".into())}
        i+=1;ws(b,&mut i);
        let v=if b.get(i)==Some(&b'"'){V::S(quoted(b,&mut i)?)}else{
            let start=i;
            while i<b.len()&&b[i].is_ascii_digit(){i+=1}
            if start==i||i-start>1&&b[start]==b'0'{return Err("invalid JSON number".into())}
            V::N(std::str::from_utf8(&b[start..i]).unwrap().parse::<u64>()
                .map_err(|_|"JSON integer overflow")?)
        };
        if o.insert(k,v).is_some()||o.len()>12{return Err("duplicate/excess fields".into())}
        ws(b,&mut i);match b.get(i){
            Some(b',')=>i+=1,Some(b'}')=>{i+=1;break},
            _=>return Err("invalid JSON separator".into())
        }
    }
    ws(b,&mut i);
    if i!=b.len()||o.len()!=12{return Err("missing/trailing challenge fields".into())}
    Ok(o)
}
fn s<'a>(m:&'a BTreeMap<String,V>,k:&str)->Result<&'a str,String>{
    match m.get(k){Some(V::S(v))=>Ok(v),_=>Err(format!("missing string {k}"))}
}
fn n(m:&BTreeMap<String,V>,k:&str)->Result<u64,String>{
    match m.get(k){Some(V::N(v))=>Ok(*v),_=>Err(format!("missing integer {k}"))}
}
fn sha(s:&str,len:usize)->bool{s.len()==len&&s.bytes().all(|b|b.is_ascii_hexdigit()&&!b.is_ascii_uppercase())}
fn check(m:&BTreeMap<String,V>,head:&str,tree:&str,fixture:&str,now:u64)->Result<String,String>{
    for (field,expected) in [
        ("repository","atto-lang/atto-source.llvm"),("source_sha",head),
        ("tree_sha",tree),("case_sha256",fixture),("backend","llvm"),
        ("feature","actor.m4.identical_counter"),
        ("workflow",".github/workflows/m4-llvm-challenged-evidence.yml"),
        ("command","bash ci/m4/verify-shared-counter.sh")
    ]{if s(m,field)?!=expected{return Err(format!("mismatched {field}"))}}
    let issued=n(m,"issued_unix")?;let expires=n(m,"expires_unix")?;
    if n(m,"version")?!=1||issued<1_700_000_000||expires<=issued
        ||expires-issued>600||now<issued||now>=expires{
        return Err("invalid or expired challenge lifetime".into())
    }
    let nonce=s(m,"nonce")?;
    if !sha(nonce,64){return Err("invalid nonce".into())}
    Ok(nonce.to_owned())
}
fn cmd(args:&[&str])->Result<String,String>{
    let r=Command::new(args[0]).args(&args[1..]).stdin(Stdio::null())
        .stderr(Stdio::null()).output().map_err(|_|"required verifier dependency unavailable")?;
    if !r.status.success(){return Err(format!("{} failed",args[0]))}
    String::from_utf8(r.stdout).map(|v|v.trim().into()).map_err(|_|"invalid command output".into())
}
fn read(path:&str,max:u64)->Result<Vec<u8>,String>{
    let m=fs::symlink_metadata(path).map_err(|_|"challenge file missing")?;
    if !m.file_type().is_file()||m.len()>max{return Err("bad challenge file".into())}
    fs::read(path).map_err(|_|"cannot read challenge file".into())
}
fn verify(payload:&str,sig:&str,pubkey:&str,pin:&str,
    head:&str,tree:&str,fixture:&str,now:u64)->Result<String,String>{
    if !sha(pin,64){return Err("independently approved key pin missing".into())}
    let bytes=read(payload,4096)?;let sig_bytes=read(sig,64)?;let pem=read(pubkey,8192)?;
    if sig_bytes.len()!=64||!pem.starts_with(b"-----BEGIN PUBLIC KEY-----\n")
        ||!pem.ends_with(b"-----END PUBLIC KEY-----\n"){
        return Err("missing Ed25519 signature/key".into())
    }
    let observed=cmd(&["sha256sum",pubkey])?;
    if observed.split_whitespace().next()!=Some(pin){return Err("unapproved trust root".into())}
    let challenge=check(&json(&bytes)?,head,tree,fixture,now)?;
    let ok=Command::new("openssl")
        .args(["pkeyutl","-verify","-rawin","-pubin","-inkey",pubkey,
               "-in",payload,"-sigfile",sig])
        .stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null())
        .status().map_err(|_|"OpenSSL Ed25519 unavailable")?;
    if !ok.success(){return Err("verifier challenge signature rejected".into())}
    Ok(challenge)
}
fn main(){
    let a:Vec<String>=env::args().collect();
    if a.len()!=4{eprintln!("usage: accept_m4_challenge JSON SIG PUBLIC_PEM");std::process::exit(2)}
    let result=(||->Result<String,String>{
        let head=env::var("GITHUB_SHA").map_err(|_|"missing head SHA")?;
        let repo=env::var("GITHUB_REPOSITORY").map_err(|_|"missing repository")?;
        let pin=env::var("ATTO_APPROVED_CHALLENGE_KEY_SHA256")
            .map_err(|_|"approved challenge trust root missing")?;
        if repo!="atto-lang/atto-source.llvm"||!sha(&head,40)
            ||cmd(&["git","rev-parse","HEAD"])?!=head{
            return Err("wrong producer or exact head".into())
        }
        let tree=cmd(&["git","rev-parse","HEAD^{tree}"])?;
        let fixture=cmd(&["sha256sum","fixtures/m4/shared_actor_counter.atto"])?;
        let fixture=fixture.split_whitespace().next().ok_or("missing fixture digest")?;
        if !sha(&tree,40)||!sha(fixture,64){return Err("invalid source hashes".into())}
        let now=SystemTime::now().duration_since(UNIX_EPOCH)
            .map_err(|_|"bad clock")?.as_secs();
        verify(&a[1],&a[2],&a[3],&pin,&head,&tree,fixture,now)
    })();
    match result{
        Ok(nonce)=>println!("{nonce}"),
        Err(err)=>{eprintln!("NOT_CERTIFIED: {err}");std::process::exit(2)}
    }
}
#[cfg(test)]mod tests{
    use super::*;
    fn payload()->String{
        format!(concat!(
          "{{\"version\":1,\"repository\":\"atto-lang/atto-source.llvm\",",
          "\"source_sha\":\"{}\",\"tree_sha\":\"{}\",",
          "\"case_sha256\":\"{}\",\"backend\":\"llvm\",",
          "\"feature\":\"actor.m4.identical_counter\",",
          "\"workflow\":\".github/workflows/m4-llvm-challenged-evidence.yml\",",
          "\"command\":\"bash ci/m4/verify-shared-counter.sh\",",
          "\"nonce\":\"{}\",\"issued_unix\":1800000000,\"expires_unix\":1800000600}}"
        ),"a".repeat(40),"b".repeat(40),"c".repeat(64),"d".repeat(64))
    }
    fn pass(raw:&str)->bool{
        json(raw.as_bytes()).and_then(|v|check(&v,&"a".repeat(40),
            &"b".repeat(40),&"c".repeat(64),1_800_000_001)).is_ok()
    }
    #[test]fn strict_json_and_binding(){
        let p=payload();assert!(pass(&p));
        assert!(!pass(&p.replace("\"version\":1","\"version\":1,\"version\":1")));
        assert!(!pass(&p.replace("\"version\":1","\"version\":2")));
        assert!(!pass(&p.replace("actor.m4.identical_counter","actor.other")));
        assert!(!pass(&p.replace("\"backend\":\"llvm\",","")));
        assert!(!pass(&(p.clone()+"{}")));
        assert!(!pass(&p.replace("1800000600","1800000000")));
        assert!(!pass(&p.replace("\"nonce\":","\"wrong\":")));
    }
    #[test]fn expiry_future_and_wrong_head(){
        let v=json(payload().as_bytes()).unwrap();
        assert!(check(&v,&"a".repeat(40),&"b".repeat(40),
            &"c".repeat(64),1_800_000_600).is_err());
        assert!(check(&v,&"a".repeat(40),&"b".repeat(40),
            &"c".repeat(64),1_799_999_999).is_err());
        assert!(check(&v,&"f".repeat(40),&"b".repeat(40),
            &"c".repeat(64),1_800_000_001).is_err());
    }
    #[test]fn absent_signature_or_key_fails_closed(){
        let fake_pin="e".repeat(64);
        assert!(verify("/nonexistent","/nonexistent","/nonexistent",&fake_pin,
            &"a".repeat(40),&"b".repeat(40),&"c".repeat(64),1_800_000_001).is_err());
        assert!(verify("/nonexistent","/nonexistent","/nonexistent","NOT_CONFIGURED",
            &"a".repeat(40),&"b".repeat(40),&"c".repeat(64),1_800_000_001).is_err());
    }
}
