//! CLI smoke tests: the declared attoc bin must exist, compile, and fail closed.
use std::{fs,process::Command,time::{SystemTime,UNIX_EPOCH}};

#[test]
fn cli_emits_checked_wasm_and_no_artifact_on_rejection() {
    let dir=std::env::temp_dir().join(format!("attoc-cli-{}-{}",std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));
    fs::create_dir_all(&dir).unwrap();
    let source=dir.join("input.atto");
    let wasm=dir.join("out.wasm");
    fs::write(&source,"pub fnc main(): int { do sync task<void> {}; return 7; }").unwrap();
    // The frontend CLI must retain the old no--output stdout contract.
    let streamed=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).output().unwrap();
    assert!(streamed.status.success(), "{}", String::from_utf8_lossy(&streamed.stderr));
    assert!(streamed.stdout.starts_with(b"\0asm"));
    assert!(!wasm.exists(), "stdout mode unexpectedly created an artifact");
    let pass=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&wasm).output().unwrap();
    assert!(pass.status.success(),"{}",String::from_utf8_lossy(&pass.stderr));
    assert!(fs::read(&wasm).unwrap().starts_with(b"\0asm"));
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = fs::metadata(&wasm).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode, 0o600, "guest artifact was world/group-readable");
    }
    // A second invocation must never clobber an existing artifact.
    let first_bytes=fs::read(&wasm).unwrap();
    let existing=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&wasm).output().unwrap();
    assert!(!existing.status.success(), "existing artifact was replaced");
    assert_eq!(fs::read(&wasm).unwrap(), first_bytes);
    let original_source=fs::read(&source).unwrap();
    let alias=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&source).output().unwrap();
    assert!(!alias.status.success(), "source input was overwritten");
    assert_eq!(fs::read(&source).unwrap(), original_source);
    fs::remove_file(&wasm).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::symlink;
        let victim = dir.join("victim.bin");
        fs::write(&victim, b"victim-preserved").unwrap();
        symlink(&victim, &wasm).unwrap();
        let symlink_output = Command::new(env!("CARGO_BIN_EXE_attoc"))
            .arg(&source).arg("-o").arg(&wasm).output().unwrap();
        assert!(!symlink_output.status.success(), "existing output symlink was replaced");
        assert_eq!(fs::read(&victim).unwrap(), b"victim-preserved");
        assert!(fs::symlink_metadata(&wasm).unwrap().file_type().is_symlink());
        fs::remove_file(&wasm).unwrap();
    }
    fs::write(&source,"pub fnc main(): int { do sync task<int> {}; return 7; }").unwrap();
    let fail=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&wasm).output().unwrap();
    assert!(!fail.status.success());
    assert!(String::from_utf8_lossy(&fail.stderr).contains("E_TASK_RETURN_TYPE"));
    assert!(!wasm.exists(),"diagnostic left behind a partial wasm artifact");
    let invalid_stdout = Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source)
        .output().unwrap();
    assert!(!invalid_stdout.status.success());
    assert!(invalid_stdout.stdout.is_empty(), "diagnostic polluted binary stdout");
    assert!(String::from_utf8_lossy(&invalid_stdout.stderr).contains("E_TASK_RETURN_TYPE"));
    assert!(fs::read_dir(&dir).unwrap().all(|x| !x.unwrap().file_name().to_string_lossy().contains(".partial.")));
    fs::remove_dir_all(dir).unwrap();
}
