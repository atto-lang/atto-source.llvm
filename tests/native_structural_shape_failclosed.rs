//! Actual oreslang-llvmc admission test: native structural shapes fail closed.
//! No mocks, no private-source mirroring, and no Python. An actual scalar
//! source must compile to LLVM IR before a negative admission can pass.
use std::fs::{self, read_dir};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct TestDir(PathBuf);
impl Drop for TestDir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
impl TestDir {
    fn create() -> Self {
        let nanos = SystemTime::now().duration_since(UNIX_EPOCH).expect("clock").as_nanos();
        let dir = std::env::temp_dir().join(format!("oreslang-structural-{}-{}", std::process::id(), nanos));
        fs::create_dir(&dir).expect("create temporary test directory");
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            fs::set_permissions(&dir, fs::Permissions::from_mode(0o700)).expect("secure test directory");
        }
        Self(dir)
    }
}
fn compile(compiler: &Path, tmp: &Path, input: &str) -> std::process::Output {
    let source = tmp.join("probe.ores");
    fs::write(&source, input).expect("write fixture");
    let result = Command::new(compiler).arg(&source)
        .current_dir(tmp).output().expect("execute real native compiler");
    assert_eq!(fs::read_to_string(&source).expect("read fixture"), input, "compiler modified source");
    let unexpected:Vec<_> = read_dir(tmp).expect("list artifacts")
        .map(|e| e.expect("directory entry").file_name())
        .filter(|name| name != "probe.ores").collect();
    assert!(unexpected.is_empty(), "compiler published artifacts during admission: {:?}", unexpected);
    result
}
fn main() {
    let compiler = std::env::args_os().nth(1).expect("usage: native_structural_shape_failclosed COMPILER_PATH");
    let compiler = PathBuf::from(compiler).canonicalize().expect("compiler must exist");
    let tmp = TestDir::create();
    let positive = compile(&compiler, &tmp.0, "pub fnc main(): int { return 42; }\n");
    assert!(positive.status.success() && positive.stdout.starts_with(b"; ModuleID"),
        "real scalar control must emit LLVM ModuleID: status={:?}, stderr={}",
        positive.status, String::from_utf8_lossy(&positive.stderr));
    let probes = [
        ("interface parameter", "define interface HasId\n id:int;\nend\nfnc take(structural HasId v):int{return v.id;}\n"),
        ("inline record parameter", "fnc take(structural { id:int } v):int{return v.id;}\n"),
        ("named shape pattern", "fnc main():int { match 1 over on structural HasId v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("inline shape pattern", "fnc main():int { match 1 over on structural { id:int } v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("tuple parameter", "fnc take(structural Tuple<int,string> v):int{return v[0];}\n"),
        ("tuple pattern", "fnc main():int { match 1 over on structural (int,int) v -> { return 1; } on _ -> { return 0; } end }\n"),
        ("actor parameter", "actor fnc take(structural { id:int } v):int{return 1;}\n")
    ];
    for (name, src) in probes {
        let output = compile(&compiler, &tmp.0, src);
        assert!(!output.status.success(), "{name}: unsupported structural program compiled");
        assert!(output.stdout.iter().all(u8::is_ascii_whitespace), "{name}: emitted LLVM IR on rejection");
        assert!(!output.stderr.is_empty(), "{name}: missing diagnostic");
    }
    println!("PASS: 1 native scalar positive control; {} structural negative, no-IR/no-artifact checks", probes.len());
}
