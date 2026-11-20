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
    let pass=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&wasm).output().unwrap();
    assert!(pass.status.success(),"{}",String::from_utf8_lossy(&pass.stderr));
    assert!(fs::read(&wasm).unwrap().starts_with(b"\0asm"));
    fs::remove_file(&wasm).unwrap();
    fs::write(&source,"pub fnc main(): int { do sync task<int> {}; return 7; }").unwrap();
    let fail=Command::new(env!("CARGO_BIN_EXE_attoc")).arg(&source).arg("-o")
        .arg(&wasm).output().unwrap();
    assert!(!fail.status.success());
    assert!(String::from_utf8_lossy(&fail.stderr).contains("E_TASK_RETURN_TYPE"));
    assert!(!wasm.exists(),"diagnostic left behind a partial wasm artifact");
    assert!(fs::read_dir(&dir).unwrap().all(|x| !x.unwrap().file_name().to_string_lossy().contains(".partial.")));
    fs::remove_dir_all(dir).unwrap();
}
