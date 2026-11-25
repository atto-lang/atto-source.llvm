use std::{
    env,
    fs,
    path::{Path, PathBuf},
    process::Command,
};

fn run(compiler: &Path, fixture: &Path) -> Result<(bool, String, String), String> {
    let output = Command::new(compiler)
        .arg(fixture)
        .output()
        .map_err(|e| format!("execute compiler for {}: {e}", fixture.display()))?;
    let stdout = String::from_utf8(output.stdout)
        .map_err(|e| format!("{} stdout is not UTF-8: {e}", fixture.display()))?;
    let stderr = String::from_utf8(output.stderr)
        .map_err(|e| format!("{} stderr is not UTF-8: {e}", fixture.display()))?;
    Ok((output.status.success(), stdout, stderr))
}

fn positive(
    compiler: &Path,
    root: &Path,
    fixture: &str,
    required: &[&str],
    forbidden: &[&str],
) -> Result<(), String> {
    let path = root.join(fixture);
    let (ok, stdout, stderr) = run(compiler, &path)?;
    if !ok {
        return Err(format!(
            "{fixture}: expected compiler success; stderr={stderr:?}"
        ));
    }
    if stdout.trim().is_empty() {
        return Err(format!("{fixture}: successful compile emitted no LLVM IR"));
    }
    for needle in required {
        if !stdout.contains(needle) {
            return Err(format!(
                "{fixture}: expected LLVM IR fragment {needle:?}; stdout={stdout:?}"
            ));
        }
    }
    for needle in forbidden {
        if stdout.contains(needle) {
            return Err(format!(
                "{fixture}: forbidden LLVM IR fragment {needle:?}; stdout={stdout:?}"
            ));
        }
    }
    Ok(())
}

fn reject(
    compiler: &Path,
    root: &Path,
    fixture: &str,
    diagnostic: &str,
) -> Result<(), String> {
    let path = root.join(fixture);
    let (ok, stdout, stderr) = run(compiler, &path)?;
    if ok {
        return Err(format!("{fixture}: expected compiler rejection"));
    }
    if !stdout.trim().is_empty() {
        return Err(format!(
            "{fixture}: rejected source emitted stdout/LLVM IR: {stdout:?}"
        ));
    }
    if !stderr.contains(diagnostic) {
        return Err(format!(
            "{fixture}: expected diagnostic {diagnostic:?}; stderr={stderr:?}"
        ));
    }
    Ok(())
}

fn require_fixture(root: &Path, fixture: &str) -> Result<(), String> {
    let path = root.join(fixture);
    let meta = fs::metadata(&path)
        .map_err(|e| format!("missing fixture {}: {e}", path.display()))?;
    if !meta.is_file() {
        return Err(format!("fixture is not a regular file: {}", path.display()));
    }
    Ok(())
}

fn main() {
    let mut args = env::args_os();
    let _program = args.next();
    let Some(compiler) = args.next() else {
        eprintln!("native sealed-state conditional contract failed: expected compiler path");
        std::process::exit(2);
    };
    let Some(root) = args.next() else {
        eprintln!("native sealed-state conditional contract failed: expected fixture root");
        std::process::exit(2);
    };
    if args.next().is_some() {
        eprintln!("native sealed-state conditional contract failed: unexpected extra arguments");
        std::process::exit(2);
    }
    let compiler = PathBuf::from(compiler);
    let root = PathBuf::from(root);

    let fixtures = [
        "sealed-state-schema-20-positive.ores",
        "sealed-state-schema-33-reject.ores",
        "sealed-state-conditional-positive.ores",
        "sealed-state-relational-positive.ores",
        "sealed-state-release-positive.ores",
        "sealed-state-conditional-readonly-reject.ores",
        "sealed-state-conditional-missing-read-reject.ores",
        "sealed-state-definite-write-both-arms-positive.ores",
        "sealed-state-definite-write-one-arm-snapshot.ores",
    ];
    for fixture in fixtures {
        if let Err(err) = require_fixture(&root, fixture) {
            eprintln!("native sealed-state conditional contract failed: {err}");
            std::process::exit(1);
        }
    }

    let checks = [
        positive(
            &compiler,
            &root,
            "sealed-state-schema-20-positive.ores",
            &["call i64 @ores_actor_i64_heap_field_read"],
            &[],
        ),
        reject(
            &compiler,
            &root,
            "sealed-state-schema-33-reject.ores",
            "native actor state field limit exceeded (32)",
        ),
        positive(
            &compiler,
            &root,
            "sealed-state-conditional-positive.ores",
            &["state.if.then"],
            &[],
        ),
        positive(
            &compiler,
            &root,
            "sealed-state-relational-positive.ores",
            &["icmp sle i64"],
            &[],
        ),
        positive(
            &compiler,
            &root,
            "sealed-state-release-positive.ores",
            &["sub i64"],
            &[],
        ),
        reject(
            &compiler,
            &root,
            "sealed-state-conditional-readonly-reject.ores",
            "read-only actor state loan cannot mutate state",
        ),
        reject(
            &compiler,
            &root,
            "sealed-state-conditional-missing-read-reject.ores",
            "state callback reads state but with bound omits state.read",
        ),
        positive(
            &compiler,
            &root,
            "sealed-state-definite-write-both-arms-positive.ores",
            &["call void @ores_actor_i64_heap_field_write"],
            &["call i64 @ores_actor_i64_heap_field_read"],
        ),
        positive(
            &compiler,
            &root,
            "sealed-state-definite-write-one-arm-snapshot.ores",
            &["call i64 @ores_actor_i64_heap_field_read"],
            &[],
        ),
    ];

    for result in checks {
        if let Err(err) = result {
            eprintln!("native sealed-state conditional contract failed: {err}");
            std::process::exit(1);
        }
    }

    println!("PASS: 9 sealed-state conditional/merge contracts");
}
