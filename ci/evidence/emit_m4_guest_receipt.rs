//! Source-owned LLVM M4 guest receipt emitter. UNSIGNED, NOT_CERTIFIED.
use std::{env,fs,path::Path,process::Command};
const OUTPUT:&str="ci/guest-evidence";
const OBS:&str="build/atto-tenant-full-source/m4-observations.txt";
const FIXTURE:&str="fixtures/m4/shared_actor_counter.atto";
const REQUIRED:[&str;6]=[
"ATTO_M4_LLVM_SOURCE_GUEST_RETURN=43",
"ATTO_M4_LLVM_ACTOR_CALLS_OBSERVED=3",
"ATTO_M4_LLVM_OBSERVATION_ACTOR_STOPS=2",
"ATTO_M4_LLVM_HOST_OBSERVED_ACTOR_DIGEST=7021",
"ATTO_M4_LLVM_MAIN_GUEST_DRAINED=1",
"ATTO_M4_SCOPE=ACTUAL_LLVM_TENANT_GUEST_NOT_PRODUCTION_SANDBOX",
];
fn sha(s:&str,n:usize)->bool{
    s.len()==n&&s.bytes().all(|b|b.is_ascii_hexdigit()&&!b.is_ascii_uppercase())
}
fn digits(s:&str)->bool{
    !s.is_empty()&&s.bytes().all(|b|b.is_ascii_digit())
      &&s.parse::<u64>().map(|v|v>0).unwrap_or(false)
}
fn transcript(buf:&[u8])->Result<(),String>{
    if buf.len()>8192{return Err("guest transcript too large".into())}
    let t=std::str::from_utf8(buf).map_err(|_|"non UTF-8 transcript")?;
    let lines:Vec<_>=t.lines().collect();
    if lines.len()!=REQUIRED.len()||REQUIRED.iter().any(|e|!lines.contains(e)){
        return Err("native guest transcript missing exact expected assertions".into())
    }
    Ok(())
}
fn command(args:&[&str])->Result<String,String>{
    let output=Command::new(args[0]).args(&args[1..])
        .output().map_err(|e|e.to_string())?;
    if !output.status.success(){return Err(format!("{} failed",args[0]));}
    String::from_utf8(output.stdout).map(|x|x.trim().to_owned())
        .map_err(|e|e.to_string())
}
fn env_var(name:&str)->Result<String,String>{
    env::var(name).map_err(|_|format!("missing {name}"))
}
fn emit()->Result<(),String>{
    let repo=env_var("GITHUB_REPOSITORY")?;
    let source=env_var("GITHUB_SHA")?;
    let run=env_var("GITHUB_RUN_ID")?;
    let attempt=env_var("GITHUB_RUN_ATTEMPT")?;
    let job=env_var("ATTO_NUMERIC_JOB_ID")?;
    let challenge=env_var("ATTO_GUEST_NONCE")?;
    if repo!="atto-lang/atto-source.llvm"||!sha(&source,40)
      ||!digits(&run)||!digits(&attempt)||!digits(&job)
      ||!sha(&challenge,64){
        return Err("untrusted GitHub run/job/source/nonce inputs".into())
    }
    let local=command(&["git","rev-parse","HEAD"])?;
    if source!=local{return Err("not exact head".into())}
    let tree=command(&["git","rev-parse","HEAD^{tree}"])?;
    if !sha(&tree,40){return Err("invalid Git tree SHA".into())}
    if !Command::new("git").args(["diff","--quiet","HEAD","--"])
        .status().map_err(|e|e.to_string())?.success(){
        return Err("tracked source tree changed".into())
    }
    let output=fs::read(OBS).map_err(|e|e.to_string())?;
    transcript(&output)?;
    let fixture=command(&["sha256sum",FIXTURE])?;
    let fixture_sha=fixture.split_whitespace().next().ok_or("fixture SHA absent")?;
    let stdout=command(&["sha256sum",OBS])?;
    let stdout_sha=stdout.split_whitespace().next().ok_or("stdout SHA absent")?;
    if !sha(fixture_sha,64)||!sha(stdout_sha,64){
        return Err("invalid SHA256 observation".into())
    }
    let receipt=format!(concat!(
      "{{\"version\":1,\"repository\":\"{}\",\"source_sha\":\"{}\",",
      "\"tree_sha\":\"{}\",\"case_sha256\":\"{}\",\"backend\":\"llvm\",",
      "\"feature\":\"actor.m4.identical_counter\",\"run_id\":{},",
      "\"run_attempt\":{},\"job_id\":{},\"target\":\"guest\",",
      "\"command\":\"bash ci/m4/verify-shared-counter.sh\",",
      "\"nonce\":\"{}\",\"executed\":1,\"failed\":0,\"exit_code\":0,",
      "\"stdout_sha256\":\"{}\"}}\n"
    ),repo,source,tree,fixture_sha,run,attempt,job,challenge,stdout_sha);
    fs::create_dir_all(Path::new(OUTPUT)).map_err(|e|e.to_string())?;
    fs::write(format!("{OUTPUT}/guest-receipt.json"),receipt).map_err(|e|e.to_string())?;
    fs::write(format!("{OUTPUT}/guest-stdout.bin"),output).map_err(|e|e.to_string())?;
    println!("M4_LLVM_OBSERVATION=UNSIGNED_NOT_CERTIFIED");
    println!("M4_LLVM_SOURCE_HEAD_SHA={source}");
    println!("M4_LLVM_SOURCE_TREE_SHA={tree}");
    println!("M4_LLVM_FIXTURE_SHA256={fixture_sha}");
    println!("M4_LLVM_GUEST_STDOUT_SHA256={stdout_sha}");
    Ok(())
}
fn main(){
    if let Err(e)=emit(){eprintln!("M4_LLVM_NOT_CERTIFIED: {e}");std::process::exit(2)}
}
#[cfg(test)]mod tests{
    use super::*;
    #[test]fn bad_provenance_rejected(){
        assert!(!sha("f",40));
        assert!(!sha(&"F".repeat(40),40));
        assert!(!sha(&"z".repeat(40),40));
        assert!(!digits("0"));assert!(!digits("12x"));assert!(!digits("-1"));
    }
    #[test]fn exact_native_transcript_required(){
        let valid=REQUIRED.join("\n")+"\n";
        assert!(transcript(valid.as_bytes()).is_ok());
        assert!(transcript(valid.replace("=43","=42").as_bytes()).is_err());
        assert!(transcript(format!("{valid}SURPLUS\n").as_bytes()).is_err());
        assert!(transcript(b"\xff").is_err());
    }
}
