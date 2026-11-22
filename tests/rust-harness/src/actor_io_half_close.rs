//! Run the real C++ actor-I/O host regression from a Rust test harness.
//! This is native-runtime evidence only; no .atto guest compile or AOT parity claim.
use std::{
    env,
    fs,
    path::PathBuf,
    process::Command,
    sync::atomic::{AtomicU64, Ordering},
    time::{SystemTime, UNIX_EPOCH},
};

static SEQUENCE: AtomicU64 = AtomicU64::new(0);

#[test]
fn native_actor_io_half_close_executes_o0_o2() {
    let root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let source = root.join("tests/native_actor_io_envelope_test.cpp");
    let include = root.join("src/main/cpp");
    assert!(source.is_file(), "missing native actor-I/O executable regression");
    let unique = format!(
        "ores-native-actor-io-{}-{}-{}",
        std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos(),
        SEQUENCE.fetch_add(1, Ordering::Relaxed)
    );
    let directory = env::temp_dir().join(unique);
    fs::create_dir(&directory).expect("create isolated native regression build directory");
    let compiler = env::var_os("CXX").unwrap_or_else(|| "c++".into());
    for level in ["-O0", "-O2"] {
        let executable = directory.join(if level == "-O0" { "actor-io-o0" } else { "actor-io-o2" });
        let build = Command::new(&compiler)
            .args(["-std=c++17", "-Wall", "-Wextra", "-pedantic", level])
            .arg("-I").arg(&include).arg(&source)
            .arg("-o").arg(&executable)
            .output()
            .expect("invoke native C++ compiler");
        assert!(build.status.success(),
            "native actor-I/O build {level} failed: {}",
            String::from_utf8_lossy(&build.stderr));
        let run = Command::new(&executable)
            .output()
            .expect("execute compiled native actor-I/O regression");
        assert!(run.status.success(),
            "native actor-I/O execution {level} failed, exit {:?}; stderr: {}; stdout: {}",
            run.status.code(),
            String::from_utf8_lossy(&run.stderr),
            String::from_utf8_lossy(&run.stdout));
    }
    fs::remove_dir_all(directory).expect("remove native regression build artifacts");
}
