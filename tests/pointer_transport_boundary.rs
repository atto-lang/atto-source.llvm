//! Exact compiler execution: unsupported pointer operations must emit no IR.
use std::{env, fs, process::Command};

fn main() {
    let compiler = env::args().nth(1).expect("compiler path required");
    let root = env::temp_dir().join(format!("ores-pointer-boundary-{}", std::process::id()));
    fs::create_dir(&root).expect("fresh fixture directory");
    let mut count = 0;
    for operation in ["ref", "unref", "deref", "deref mut"] {
        for expression in [
            format!("rt {operation} value"),
            format!("rt {operation}(value)"),
            format!("rt copy (rt {operation} value)"),
            format!("rt take (rt {operation} value)"),
            format!("rt share (rt {operation} value)"),
        ] {
            let path = root.join(format!("negative-{count}.ores"));
            fs::write(&path, format!("pub fnc main(): int {{ return {expression}; }}")).unwrap();
            let output = Command::new(&compiler).arg(&path).output().expect("execute compiler");
            assert!(!output.status.success(), "accepted {expression}");
            assert!(output.stdout.is_empty(), "partial IR emitted for {expression}");
            let error = String::from_utf8_lossy(&output.stderr);
            assert!(error.contains("requires provenance-checked Pointer<T>"), "{expression}: {error}");
            count += 1;
        }
    }
    let path = root.join("positive.ores");
    fs::write(&path, "pub fnc main(): int { return 42; }").unwrap();
    let output = Command::new(&compiler).arg(&path).output().unwrap();
    assert!(output.status.success(), "positive compiler control failed: {}", String::from_utf8_lossy(&output.stderr));
    assert!(String::from_utf8_lossy(&output.stdout).contains("define"));
    fs::remove_dir_all(root).unwrap();
    println!("PASS: {count} pointer boundary rejections with empty IR; scalar positive control");
}
