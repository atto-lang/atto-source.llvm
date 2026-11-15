//! Compile verified LLVM IR from the ref32 lowering test and execute its checked paths.
//!
//! Run only with a clang matching the configured LLVM IR producer. Each step has
//! its own timeout so a stuck compilation fails here instead of hanging CI.

use std::env;
use std::fs::{self, File};
use std::path::{Path, PathBuf};
use std::process::{self, Command};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const STEP_TIMEOUT: Duration = Duration::from_secs(75);
const RUN_TIMEOUT: Duration = Duration::from_secs(30);
const OUTPUT_TAIL_BYTES: usize = 4000;

struct Scratch(PathBuf);

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn tail(bytes: &[u8]) -> String {
    let start = bytes.len().saturating_sub(OUTPUT_TAIL_BYTES);
    String::from_utf8_lossy(&bytes[start..]).into_owned()
}

/// Output goes to files, not pipes: polling for a timeout while a child blocks on
/// a full pipe would turn a large compiler log into a false timeout.
fn run(dir: &Path, program: &str, args: &[&str], timeout: Duration) -> Result<String, String> {
    let shown = format!("{program} {}", args.join(" "));
    let stdout_path = dir.join("step.stdout");
    let stderr_path = dir.join("step.stderr");
    let stdout = File::create(&stdout_path).map_err(|e| format!("{shown}: {e}"))?;
    let stderr = File::create(&stderr_path).map_err(|e| format!("{shown}: {e}"))?;
    let mut child = Command::new(program)
        .args(args)
        .stdout(stdout)
        .stderr(stderr)
        .spawn()
        .map_err(|e| format!("cannot start {shown}: {e}"))?;
    let started = Instant::now();
    let status = loop {
        match child.try_wait().map_err(|e| format!("{shown}: {e}"))? {
            Some(status) => break status,
            None if started.elapsed() > timeout => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("timed out after {timeout:?}: {shown}"));
            }
            None => thread::sleep(Duration::from_millis(10)),
        }
    };
    let out = fs::read(&stdout_path).map_err(|e| format!("{shown}: {e}"))?;
    let err = fs::read(&stderr_path).map_err(|e| format!("{shown}: {e}"))?;
    if !status.success() {
        return Err(format!(
            "command failed: {shown}\n{status}\nstdout:\n{}\nstderr:\n{}",
            tail(&out),
            tail(&err)
        ));
    }
    String::from_utf8(out).map_err(|e| format!("{shown}: non-UTF-8 output: {e}"))
}

fn execute(producer: &str, clang: &str, harness: &str) -> Result<String, String> {
    let suffix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| e.to_string())?
        .as_nanos();
    let dir = env::temp_dir().join(format!("oreslang-ref32-llvm-{}-{suffix}", process::id()));
    fs::create_dir(&dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    let scratch = Scratch(dir);
    let temp = scratch.0.as_path();

    let generated = run(temp, producer, &["--emit"], STEP_TIMEOUT)?;
    if !generated.contains("ref32.authenticated") {
        return Err("verified LLVM IR omitted original-generation proof validation".into());
    }
    let ir = temp.join("compact.ll");
    fs::write(&ir, &generated).map_err(|e| format!("{}: {e}", ir.display()))?;
    let (ir, object, executable) = (
        ir.to_string_lossy().into_owned(),
        temp.join("compact.o").to_string_lossy().into_owned(),
        temp.join("actor-ref32-e2e").to_string_lossy().into_owned(),
    );
    run(temp, clang, &["-x", "ir", "-c", &ir, "-o", &object], STEP_TIMEOUT)?;
    run(
        temp,
        clang,
        &[
            "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Wpedantic", "-Werror", harness,
            &object, "-o", &executable,
        ],
        STEP_TIMEOUT,
    )?;
    let executed = run(temp, &executable, &[], RUN_TIMEOUT)?;
    if !executed.contains("PASS LLVM compact i32 physical load/store") {
        return Err("compiled reference contract returned no success marker".into());
    }
    Ok(executed.trim().to_owned())
}

fn main() {
    let (mut producer, mut clang, mut harness) = (None, None, None);
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        let slot = match arg.as_str() {
            "--ir-producer" => &mut producer,
            "--clang" => &mut clang,
            "--harness" => &mut harness,
            _ => panic!("unknown argument: {arg}"),
        };
        *slot = Some(args.next().unwrap_or_else(|| panic!("{arg} requires a value")));
    }
    let producer = producer.expect("--ir-producer PATH required");
    let clang = clang.expect("--clang PATH required");
    let harness = harness.expect("--harness PATH required");
    match execute(&producer, &clang, &harness) {
        Ok(marker) => println!("{marker}"),
        Err(failure) => {
            eprintln!("FAIL compiled actor-ref32 LLVM execution: {failure}");
            process::exit(1);
        }
    }
}
