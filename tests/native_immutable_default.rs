// Compile+link+execute pinned native Oreslang guest programs. No Python,
// fake compiler or host-side arithmetic oracle is used by this runner.
use std::{
    env, fs,
    path::{Path, PathBuf},
    process::{Command, ExitStatus, Stdio},
    thread,
    time::{Duration, Instant},
};

// Never allow a bad source compiler, linker, or guest to hang this
// admission runner indefinitely or allocate unbounded host memory from pipes.
// Redirect output to files so a chatty child cannot deadlock on pipe capacity.
const PER_PROCESS_TIMEOUT: Duration = Duration::from_secs(20);
const MAX_PROCESS_OUTPUT: u64 = 1024 * 1024;

struct CheckedOutput {
    status: ExitStatus,
    stdout: Vec<u8>,
    stderr: Vec<u8>,
}
fn bounded_output(mut command: Command, dir: &Path, label: &str)
    -> Result<CheckedOutput, String>
{
    let stdout_file = dir.join(format!("{label}.stdout"));
    let stderr_file = dir.join(format!("{label}.stderr"));
    let out = fs::File::create(&stdout_file).map_err(|e| e.to_string())?;
    let err = fs::File::create(&stderr_file).map_err(|e| e.to_string())?;
    command.stdout(Stdio::from(out)).stderr(Stdio::from(err))
        .stdin(Stdio::null());
    let mut child = command.spawn().map_err(|e| format!("{label} spawn: {e}"))?;
    let until = Instant::now() + PER_PROCESS_TIMEOUT;
    let status = loop {
        if let Some(status) = child.try_wait().map_err(|e| format!("{label} wait: {e}"))? {
            break status;
        }
        if Instant::now() >= until {
            let _ = child.kill();
            let _ = child.wait();
            return Err(format!("{label}: timed out after 20s"));
        }
        thread::sleep(Duration::from_millis(20));
    };
    // The child has exited, so outputs cannot keep growing through its own
    // file descriptors. Fail closed on arbitrarily large tool output.
    for file in [&stdout_file, &stderr_file] {
        if fs::metadata(file).map_err(|e| e.to_string())?.len() > MAX_PROCESS_OUTPUT {
            return Err(format!("{label}: output exceeds 1 MiB limit"));
        }
    }
    Ok(CheckedOutput {
        status,
        stdout: fs::read(stdout_file).map_err(|e| e.to_string())?,
        stderr: fs::read(stderr_file).map_err(|e| e.to_string())?,
    })
}

struct Workspace(PathBuf);
impl Drop for Workspace {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
fn main() {
    if let Err(error) = run() {
        eprintln!("native immutable-default conformance: {error}");
        std::process::exit(1);
    }
}
fn run() -> Result<(), String> {
    let argv = env::args().collect::<Vec<_>>();
    if argv.len() != 3 {
        return Err("usage: native_immutable_default <llvm-compiler> <clang>".into());
    }
    let compiler = Path::new(&argv[1]);
    let clang = Path::new(&argv[2]);
    if !compiler.is_absolute() || !clang.is_absolute() {
        return Err("require absolute compiler and clang paths".into());
    }
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH).map_err(|e| e.to_string())?
        .as_nanos();
    let dir = env::temp_dir().join(format!(
        "oreslang-immutable-native-{}-{stamp}", std::process::id()));
    fs::create_dir(&dir).map_err(|e| e.to_string())?;
    let _workspace = Workspace(dir.clone());
    let positives: &[(&str, &str, i32)] = &[
        ("typed-default", "int x = 7; return x;", 7),
        ("const-inferred", "const x = 7; return x;", 7),
        ("const-typed", "const int x = 7; return x;", 7),
        ("const-colon", "const x: int = 7; return x;", 7),
        ("dependent-default", "int x = 7; int y = x * 3; return y;", 21),
        ("readonly-result", "int x = 7; return x + 1;", 8),
        ("bool-readonly", "bool flag = true; return 7;", 7),
        ("const-dependency", "const x = 7; const y = x + 1; return y;", 8),
        ("let-rebinding-control", "let int x = 7; x = 8; return x;", 8),
    ];
    let negatives: &[(&str, &str)] = &[
        ("rebind", "int x = 7; x = 8; return x;"),
        ("compound", "int x = 7; x += 1; return x;"),
        ("type-mismatch", "int x = true; return 7;"),
        ("unknown", "int x = missing_value; return x;"),
        ("self-reference", "int x = x + 1; return x;"),
        ("duplicate", "int x = 7; int x = 8; return x;"),
        ("missing-initializer", "int x; return 7;"),
        ("const-rebind", "const x = 7; x = 8; return x;"),
        ("bool-mismatch", "bool flag = 7; return 7;"),
    ];
    for (name, body, expected) in positives {
        let src = dir.join(format!("{name}.ores"));
        let ir = dir.join(format!("{name}.ll"));
        let exe = dir.join(format!("{name}.app"));
        fs::write(&src, format!("pub fnc main(): int {{ {body} }}\n"))
            .map_err(|e| e.to_string())?;
        let mut compile_cmd = Command::new(compiler);
        compile_cmd.arg(&src);
        let output = bounded_output(compile_cmd, &dir, &format!("{name}-compile"))?;
        if !output.status.success() || output.stdout.is_empty() {
            return Err(format!("{name} did not emit real IR: {}",
                String::from_utf8_lossy(&output.stderr)));
        }
        if output.stdout.len() > 8 * 1024 * 1024 {
            return Err(format!("{name} emitted oversized IR"));
        }
        fs::write(&ir, &output.stdout).map_err(|e| e.to_string())?;
        let mut link_cmd = Command::new(clang);
        link_cmd.args(["-x", "ir"]).arg(&ir).arg("-o").arg(&exe);
        let linked = bounded_output(link_cmd, &dir, &format!("{name}-link"))?;
        if !linked.status.success() {
            return Err(format!("{name} native link failed: {}",
                String::from_utf8_lossy(&linked.stderr)));
        }
        let guest = bounded_output(
            Command::new(&exe), &dir, &format!("{name}-guest"))?;
        if guest.status.code() != Some(*expected) {
            return Err(format!("{name}: real native exit {:?}, expected {expected}; stdout={} stderr={}",
                guest.status.code(), String::from_utf8_lossy(&guest.stdout),
                String::from_utf8_lossy(&guest.stderr)));
        }
        println!("GUEST_PASS {name} native_exit={expected}");
    }
    for (name, body) in negatives {
        let src = dir.join(format!("reject-{name}.ores"));
        fs::write(&src, format!("pub fnc main(): int {{ {body} }}\n"))
            .map_err(|e| e.to_string())?;
        let mut reject_cmd = Command::new(compiler);
        reject_cmd.arg(&src);
        let result = bounded_output(reject_cmd, &dir, &format!("{name}-reject"))?;
        if result.status.success() || !result.stdout.is_empty() || result.stderr.is_empty() {
            return Err(format!("{name}: negative input was accepted, emitted IR, or had no diagnostic"));
        }
        println!("COMPILE_REJECT {name}");
    }
    println!("NATIVE_IMMUTABLE_DEFAULT_DONE positive={} negative={}",
        positives.len(), negatives.len());
    Ok(())
}
