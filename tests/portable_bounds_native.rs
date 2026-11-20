// Actual .atto source -> LLVM IR -> Clang -O0/-O2 -> executed native guest.
// This test proves nothing when the real compiler or clang is unavailable.
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
};

const PINNED_BOUNDS_BLOB: &str = "a8ae98d0de39596ffad3a00be02d8f1e255de777";

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock before epoch").as_nanos();
        let dir = env::temp_dir().join(format!("atto-llvm-bounds-{}-{nonce}", std::process::id()));
        fs::create_dir(&dir).expect("create new isolated test directory");
        Self(dir)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn command_output(mut command: Command) -> Output {
    command.output().expect("required trusted tool did not launch")
}
fn regular_source(path: &Path) {
    let metadata = fs::symlink_metadata(path).expect("missing canonical source");
    assert!(metadata.file_type().is_file(), "symlinks/nonregular source rejected: {}", path.display());
}
fn compiler_run(compiler: &Path, source: &Path) -> Output {
    let mut command = Command::new(compiler);
    command.arg("--target=host").arg(source);
    command_output(command)
}

fn main() {
    let args: Vec<_> = env::args_os().collect();
    assert_eq!(args.len(), 4, "usage: verifier COMPILER CLANG FIXTURE_DIR");
    let compiler = PathBuf::from(&args[1]);
    let clang = PathBuf::from(&args[2]);
    let fixtures = PathBuf::from(&args[3]);
    for name in ["bounds.atto", "main.atto", "invalid.atto"] {
        regular_source(&fixtures.join(name));
    }
    regular_source(&compiler);
    regular_source(&clang);

    let mut hash = Command::new("git");
    hash.arg("hash-object").arg("--").arg(fixtures.join("bounds.atto"));
    let observed = command_output(hash);
    assert!(observed.status.success(), "git hash-object must succeed");
    assert_eq!(
        String::from_utf8_lossy(&observed.stdout).trim(),
        PINNED_BOUNDS_BLOB,
        "canonical bounds.atto differs from Java and central conformance source"
    );

    let work = Scratch::new();
    fs::copy(fixtures.join("bounds.atto"), work.0.join("bounds.atto"))
        .expect("copy exact canonical source into isolated guest directory");
    fs::copy(fixtures.join("main.atto"), work.0.join("main.atto"))
        .expect("copy positive guest");
    let compiled = compiler_run(&compiler, &work.0.join("main.atto"));
    assert!(
        compiled.status.success(),
        "real compiler rejected positive imported bounds guest: {}",
        String::from_utf8_lossy(&compiled.stderr)
    );
    let ir = String::from_utf8(compiled.stdout).expect("LLVM IR must be UTF-8");
    assert!(ir.contains("@main("), "positive compilation missing native main");
    assert!(ir.len() > 16, "compiler emitted suspiciously empty IR");
    let ir_path = work.0.join("main.ll");
    fs::write(&ir_path, ir).expect("write actual emitted IR");
    for opt in ["-O0", "-O2"] {
        let exe = work.0.join(format!("bounds-{}", opt.trim_start_matches('-')));
        let mut link = Command::new(&clang);
        link.arg(opt).args(["-x", "ir"]).arg(&ir_path).arg("-o").arg(&exe);
        let linked = command_output(link);
        assert!(
            linked.status.success(),
            "clang {opt} failed to link actual guest: {}",
            String::from_utf8_lossy(&linked.stderr)
        );
        let native = command_output(Command::new(&exe));
        assert_eq!(native.status.code(), Some(42), "native {opt} guest exit mismatch");
        assert!(native.stdout.is_empty() && native.stderr.is_empty(),
            "unexpected guest stdout/stderr for {opt}");
        println!("ATTO_BOUNDS_LLVM_NATIVE_GUEST={opt}:exit42");
    }

    fs::copy(fixtures.join("invalid.atto"), work.0.join("invalid.atto"))
        .expect("copy negative source");
    let negative = compiler_run(&compiler, &work.0.join("invalid.atto"));
    assert!(!negative.status.success(), "wrong-type bounds call compiled unexpectedly");
    assert!(negative.stdout.is_empty(), "invalid source emitted executable/partial IR");
    assert!(!negative.stderr.is_empty(), "invalid source must emit a diagnostic");
    println!("ATTO_BOUNDS_LLVM_NEGATIVE=REJECTED_NO_IR");
}
