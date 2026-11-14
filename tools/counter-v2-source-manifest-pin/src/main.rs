// Rust-only immutable source-manifest admission. NOT guest compiler execution.
// The canonical JSON blob is pinned to the .NET #502 exact-head corpus.
// Parsing and guest execution are separate future LLVM compiler gates.
use std::{fs, path::Path, process::{Command, Stdio}, io::Write};

const CORPUS_GIT_BLOB: &str = "526595b444aaf761dc75c44ad9b39a08d45341aa";

fn git_blob(data: &[u8]) -> Result<String, String> {
    let mut child = Command::new("git").args(["hash-object", "--stdin"])
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped())
        .spawn().map_err(|e| format!("cannot launch git hash-object: {e}"))?;
    child.stdin.take().ok_or("git stdin absent")?.write_all(data)
        .map_err(|e| format!("git stdin write failed: {e}"))?;
    let out = child.wait_with_output().map_err(|e| format!("git hash failed: {e}"))?;
    if !out.status.success() { return Err(format!("git hash failed: {}", String::from_utf8_lossy(&out.stderr))); }
    let hash = String::from_utf8(out.stdout).map_err(|e| format!("invalid git output: {e}"))?;
    let hash = hash.trim();
    if hash.len() != 40 || !hash.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("git returned invalid blob SHA".into());
    }
    Ok(hash.into())
}

fn verify(data: &[u8]) -> Result<(), String> {
    if data.is_empty() || data.len() > 65_536 || data.starts_with(&[0xef,0xbb,0xbf]) {
        return Err("invalid corpus bytes".into());
    }
    if git_blob(data)? != CORPUS_GIT_BLOB {
        return Err("canonical seven-source JSON blob mismatch".into());
    }
    Ok(())
}

fn main() -> Result<(), String> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let path = root.join("../../contracts/actor-counter-identical-source-v1/corpus.json");
    let data = fs::read(&path).map_err(|e| format!("read corpus failed: {e}"))?;
    verify(&data)?;
    let mut mutated = data.clone();
    mutated[0] ^= 1;
    if verify(&mutated).is_ok() { return Err("mutated manifest admitted".into()); }
    let mut truncated = data.clone();
    truncated.pop();
    if verify(&truncated).is_ok() { return Err("truncated manifest admitted".into()); }
    println!("LLVM_COUNTER_V2_SOURCE_MANIFEST_PIN_PASS canonical_git_blob={} negative_mutations=2 guest_executed=false", CORPUS_GIT_BLOB);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn canonical_git_blob_sha_is_well_formed() {
        assert_eq!(CORPUS_GIT_BLOB.len(), 40);
        assert!(CORPUS_GIT_BLOB.bytes().all(|b| b.is_ascii_hexdigit()));
    }
}
