//! Source-owned Rust-only native LLVM guest smoke for the deliberately
//! small Java/LLVM/Wasm/Cranelift/.NET signed64 scalar intersection.
//! The expressions are independently authored, not copied Orestra or
//! Oreslana source. No imports, classes, actors, storage, or network claims.
//!
//! Build: rustc --edition=2021 -D warnings tools/portable_app_scalar_gate.rs -o /tmp/ores-scalar-gate
//! Run:   /tmp/ores-scalar-gate /absolute/path/oreslang-llvmc /absolute/path/clang
//! The caller must independently attest source/compiler hashes, executed
//! CI steps, and code provenance. This is NOT a source-free test runner.
use std::env;
use std::ffi::OsString;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::time::{SystemTime, UNIX_EPOCH};

struct Workspace(PathBuf);

impl Workspace {
    fn create() -> Result<Self, String> {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| format!("invalid wall clock: {e}"))?
            .as_nanos();
        let path = env::temp_dir().join(format!(
            "oreslang-native-scalar-{}-{nanos}", std::process::id()
        ));
        fs::create_dir(&path).map_err(|e| format!("cannot create workspace: {e}"))?;
        Ok(Self(path))
    }
}
impl Drop for Workspace {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn checked_binary(name: &str, candidate: OsString) -> Result<PathBuf, String> {
    let path = PathBuf::from(candidate);
    if !path.is_absolute() {
        return Err(format!("{name}: require an explicit absolute binary path"));
    }
    let meta = fs::symlink_metadata(&path)
        .map_err(|e| format!("{name}: unavailable executable: {e}"))?;
    if !meta.is_file() {
        return Err(format!("{name}: executable must be a regular file, not symlink"));
    }
    Ok(path)
}
fn call(binary: &Path, arguments: &[OsString]) -> Result<Output, String> {
    Command::new(binary)
        .args(arguments)
        .env_clear()
        .env("PATH", "/usr/bin:/bin")
        .output()
        .map_err(|e| format!("{}: could not execute: {e}", binary.display()))
}
fn require_success(stage: &str, output: &Output) -> Result<(), String> {
    if output.status.success() {
        return Ok(());
    }
    Err(format!(
        "{stage} failed, status={:?}, stderr={}",
        output.status.code(),
        String::from_utf8_lossy(&output.stderr)
            .chars().take(512).collect::<String>()
    ))
}
fn compile_and_run(
    compiler: &Path,
    clang: &Path,
    work: &Workspace,
    name: &str,
    expression: &str,
    expected: i32,
) -> Result<(), String> {
    if !(0..=255).contains(&expected) {
        return Err("OS entrypoint exit must fit in 0..255".to_string());
    }
    // Use one immutable candidate main at a time. Never invoke a shell,
    // substitute a host-language oracle, or accept a parse-only success.
    let source = format!("pub fnc main(): int {{ return {expression}; }}\n");
    if source.len() > 1024 {
        return Err("source too large".to_string());
    }
    let source_path = work.0.join(format!("{name}.ores"));
    let ir_path = work.0.join(format!("{name}.ll"));
    let binary_path = work.0.join(name);
    fs::write(&source_path, source.as_bytes())
        .map_err(|e| format!("{name}: source creation failed: {e}"))?;
    let compiled = call(compiler, &[source_path.clone().into_os_string()])?;
    require_success(&format!("{name} source compiler"), &compiled)?;
    if compiled.stdout.len() > 2_097_152
        || !compiled.stdout.windows(b"define i32 @main(".len())
            .any(|w| w == b"define i32 @main(")
    {
        return Err(format!("{name}: no bounded native OS main wrapper in real LLVM IR"));
    }
    fs::write(&ir_path, &compiled.stdout)
        .map_err(|e| format!("{name}: IR creation failed: {e}"))?;
    let linked = call(clang, &[
        OsString::from("-O0"),
        OsString::from("-x"),
        OsString::from("ir"),
        ir_path.into_os_string(),
        OsString::from("-o"),
        binary_path.clone().into_os_string(),
    ])?;
    require_success(&format!("{name} Clang link"), &linked)?;
    let guest = call(&binary_path, &[])?;
    if guest.status.code() != Some(expected)
        || !guest.stdout.is_empty() || !guest.stderr.is_empty()
    {
        return Err(format!(
            "{name}: guest mismatch expected exit={expected}; got status={:?}, \
             stdout_bytes={}, stderr_bytes={}",
            guest.status.code(), guest.stdout.len(), guest.stderr.len()
        ));
    }
    println!("GUEST_PASS {name} observed_exit={expected}");
    Ok(())
}
fn check_intermediate_overflow(
    compiler: &Path,
    clang: &Path,
    work: &Workspace,
    name: &str,
    expression: &str,
) -> Result<(), String> {
    let source = work.0.join(format!("{name}.ores"));
    let ir = work.0.join(format!("{name}.ll"));
    let executable = work.0.join(name);
    fs::write(&source, format!("pub fnc main(): int {{ return {expression}; }}\n"))
        .map_err(|e| format!("{name}: write failed: {e}"))?;
    let compiled = call(compiler, &[source.into_os_string()])?;
    if !compiled.status.success() {
        // Invalid grammar and unrelated parser failures must not be counted
        // as checked arithmetic success.
        let diagnostic = String::from_utf8_lossy(&compiled.stderr).to_ascii_lowercase();
        if !compiled.stdout.is_empty()
            || !(diagnostic.contains("overflow") || diagnostic.contains("checked"))
        {
            return Err(format!("{name}: compiler rejection was not a checked overflow"));
        }
        println!("GUEST_OVERFLOW_REJECT {name} compiler diagnostic");
        return Ok(());
    }
    if !compiled.stdout.windows(b"define i32 @main(".len())
        .any(|window| window == b"define i32 @main(")
    {
        return Err(format!("{name}: no native OS entrypoint after compilation"));
    }
    fs::write(&ir, &compiled.stdout)
        .map_err(|e| format!("{name}: IR write failed: {e}"))?;
    let linked = call(clang, &[
        OsString::from("-O0"), OsString::from("-x"), OsString::from("ir"),
        ir.into_os_string(), OsString::from("-o"),
        executable.clone().into_os_string(),
    ])?;
    require_success(&format!("{name} link"), &linked)?;
    let result = call(&executable, &[])?;
    if result.status.success() || !result.stdout.is_empty() {
        return Err(format!(
            "{name}: signed64 overflow was accepted or produced guest stdout"
        ));
    }
    println!("GUEST_OVERFLOW_TRAP {name} observed_status={:?}", result.status.code());
    Ok(())
}
fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    let compiler = checked_binary(
        "Oreslang LLVM compiler",
        args.next().ok_or("usage: gate ABSOLUTE_COMPILER ABSOLUTE_CLANG")?,
    )?;
    let clang = checked_binary(
        "Clang",
        args.next().ok_or("usage: gate ABSOLUTE_COMPILER ABSOLUTE_CLANG")?,
    )?;
    if args.next().is_some() {
        return Err("unexpected extra CLI argument".to_string());
    }
    let work = Workspace::create()?;
    let cases = [
        ("reservation_zero", "(1000 - 600) - 400", 0),
        ("reservation_boundary", "(9223372036854775807 - 9223372036854775700) - 65", 42),
        ("ledger_capacity_zero", "(4096 - 64) + (32 * 2) - 4096", 0),
        ("ledger_i64_bound", "(9223372036854775807 - 9223372036854775800) * 6", 42),
        ("nonzero_canary", "73", 73),
    ];
    for (name, expression, expected) in cases {
        compile_and_run(&compiler, &clang, &work, name, expression, expected)?;
    }
    check_intermediate_overflow(
        &compiler, &clang, &work,
        "intermediate_overflow_then_zero", "(9223372036854775807 + 1) * 0",
    )?;
    check_intermediate_overflow(
        &compiler, &clang, &work,
        "negative_overflow", "-9223372036854775808 - 1",
    )?;
    println!("SOURCE_NATIVE_GATE_PASS 5 positive + 2 checked-overflow negative guest cases");
    Ok(())
}
fn main() {
    if let Err(message) = run() {
        eprintln!("SOURCE_NATIVE_GATE_DENIED: {message}");
        std::process::exit(1);
    }
}
