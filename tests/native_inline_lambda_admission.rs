//! Real compiler admission check only; not aggregate guest execution.
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

struct TestDir(PathBuf);
impl Drop for TestDir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
fn compile(compiler: &Path, dir: &Path, input: &str) -> Output {
    let path = dir.join("probe.atto");
    fs::write(&path, input).expect("write source");
    let result = Command::new(compiler).arg(&path).current_dir(dir)
        .output().expect("run actual compiler");
    assert_eq!(fs::read(&path).unwrap(), input.as_bytes(), "source changed");
    let entries: Vec<_> = fs::read_dir(dir).unwrap()
        .map(|entry| entry.unwrap().file_name()).collect();
    assert_eq!(entries, vec![std::ffi::OsString::from("probe.atto")], "unexpected compiler artifact");
    result
}
fn main() {
    let compiler = PathBuf::from(std::env::args_os().nth(1).expect("compiler path"))
        .canonicalize().unwrap();
    let nonce = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
    let dir = TestDir(std::env::temp_dir().join(format!("atto-native-lambda-{}-{nonce}", std::process::id())));
    fs::create_dir(&dir.0).unwrap();
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&dir.0, fs::Permissions::from_mode(0o700)).unwrap();
    }
    // No generic unsupported-token diagnostic, no source change, no emitted IR.
    let patterns = [
        "(5,3,5) |> |(a,b,c)| -> a+b+c",
        "(9,) |> |(a,)| -> a",
        "() |> |()| -> 7",
        "(5,99,8) |> |(a,_,c)| -> a+c",
        "(1,2) |> |(a,a)| -> a",
        "(1,2) |> |(a,b,c)| -> a",
        "1 |> |(a,b)| -> a+b",
        "5 |> |a| -> a",
    ];
    for expr in patterns {
        let input = format!("pub fnc main(): int {{ return {expr}; }}\n");
        let r = compile(&compiler, &dir.0, &input);
        assert_eq!(r.status.code(), Some(1), "{expr}: unexpected status");
        assert!(r.stdout.is_empty(), "{expr}: emitted IR");
        assert!(String::from_utf8_lossy(&r.stderr).contains("NATIVE_INLINE_LAMBDA_UNSUPPORTED"), "{expr}: {}", String::from_utf8_lossy(&r.stderr));
    }
    // Ordinary logical and pipeline tokens and comments remain supported.
    let controls = [
        "pub fnc main(): int { return 42; }\n",
        "pub fnc main(): int { /* |(a,b)| -> */ return 42; }\n",
        "fnc inc(int n): int { return n+1; } pub fnc main(): int { return 41 |> inc; }\n",
        "fnc logical(): bool { return true || false; } pub fnc main(): int { return 42; }\n",
    ];
    for input in controls {
        let r = compile(&compiler, &dir.0, input);
        assert!(r.status.success() && String::from_utf8_lossy(&r.stdout).contains("define i64 @__oreslang_guest_main("), "scalar control: {input}: {}", String::from_utf8_lossy(&r.stderr));
    }
    println!("PASS: 8 explicit unsupported lambda admission cases, 4 scalar IR controls; tuple guest parity NOT established");
}
