#![cfg_attr(test, allow(dead_code))]
//! Run Oreslang's real opt-in scalar guest with Rust (no Python dependencies).
//! The emitted IR is compiled, executed, mutated, and checked against the native C oracle.
use std::env;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const MAX_OUTPUT: u64 = 16 * 1024 * 1024;
const TIMEOUT: Duration = Duration::from_secs(60);

struct Workspace(PathBuf);
impl Workspace {
    fn new() -> Result<Self, String> {
        for trial in 0..10 {
            let now = SystemTime::now().duration_since(UNIX_EPOCH)
                .map_err(|e| format!("clock failed: {e}"))?.as_nanos();
            let path = env::temp_dir().join(format!(
                "oreslang-frame-rust-{}-{now}-{trial}", std::process::id()
            ));
            match fs::create_dir(&path) {
                Ok(()) => return Ok(Self(path)),
                Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
                Err(e) => return Err(format!("workspace allocation: {e}")),
            }
        }
        Err("failed to create a unique workspace".into())
    }
    fn path(&self) -> &Path { &self.0 }
}
impl Drop for Workspace {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

struct Output { ok: bool, stdout: String, stderr: String }
fn run(command: &mut Command, work: &Path, id: &str) -> Result<Output, String> {
    // Don't wait on a child that may block writing to an unread stdout pipe.
    let stdout_path = work.join(format!("{id}.stdout"));
    let stderr_path = work.join(format!("{id}.stderr"));
    let stdout = File::create(&stdout_path).map_err(|e| format!("{id} stdout: {e}"))?;
    let stderr = File::create(&stderr_path).map_err(|e| format!("{id} stderr: {e}"))?;
    let mut child = command.stdout(Stdio::from(stdout)).stderr(Stdio::from(stderr))
        .spawn().map_err(|e| format!("{id} spawn: {e}"))?;
    let deadline = Instant::now() + TIMEOUT;
    let status = loop {
        match child.try_wait().map_err(|e| format!("{id} wait: {e}"))? {
            Some(status) => break status,
            None if Instant::now() >= deadline => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("{id} exceeded {}s timeout", TIMEOUT.as_secs()));
            }
            None => thread::sleep(Duration::from_millis(20)),
        }
    };
    for path in [&stdout_path, &stderr_path] {
        let size = fs::metadata(path).map_err(|e| format!("{id} stat: {e}"))?.len();
        if size > MAX_OUTPUT { return Err(format!("{id}: output exceeded 16 MiB")); }
    }
    Ok(Output {
        ok: status.success(),
        stdout: fs::read_to_string(stdout_path).map_err(|e| format!("{id} stdout decode: {e}"))?,
        stderr: fs::read_to_string(stderr_path).map_err(|e| format!("{id} stderr decode: {e}"))?,
    })
}

fn ensure(ok: bool, why: impl Into<String>) -> Result<(), String> {
    if ok { Ok(()) } else { Err(why.into()) }
}
fn check_success(result: &Output, what: &str) -> Result<(), String> {
    ensure(result.ok, format!("{what} failed: {}", result.stderr))
}
fn check_tokens(text: &str, tokens: &[&str], what: &str) -> Result<(), String> {
    for token in tokens { ensure(text.contains(token), format!("{what}: missing {token}"))?; }
    Ok(())
}
fn unique_replace(text: &str, needle: &str, new: &str, what: &str) -> Result<String, String> {
    let matches = text.matches(needle).count();
    ensure(matches == 1, format!("{what}: expected 1 match, found {matches}"))?;
    Ok(text.replacen(needle, new, 1))
}

// Match complete emitted-IR instructions, fail closed on absence or ambiguity.
fn replace_line<F>(ir: &str, marker: &str, mut replacement: F, what: &str)
    -> Result<String, String> where F: FnMut(&str) -> Result<String, String>
{
    let lines: Vec<&str> = ir.lines().collect();
    let indices: Vec<usize> = lines.iter().enumerate()
        .filter_map(|(i, line)| if line.contains(marker) { Some(i) } else { None }).collect();
    ensure(indices.len() == 1,
        format!("{what}: expected one IR instruction, found {}", indices.len()))?;
    let mut output: Vec<String> = lines.iter().map(|x| x.to_string()).collect();
    output[indices[0]] = replacement(lines[indices[0]])?;
    Ok(output.join("\n") + if ir.ends_with('\n') { "\n" } else { "" })
}

#[derive(Debug, Clone, Copy)]
enum Mutation { BypassFuel, DoubleCommit, DropCommit, ForgeCancel }
impl Mutation {
    const ALL: [Mutation; 4] =
        [Self::BypassFuel, Self::DoubleCommit, Self::DropCommit, Self::ForgeCancel];
    fn id(self) -> &'static str {
        match self {
            Self::BypassFuel => "bypass-dispatch-fuel",
            Self::DoubleCommit => "double-commit",
            Self::DropCommit => "drop-persisted-commit",
            Self::ForgeCancel => "cancel-status-forgery",
        }
    }
    fn expected(self) -> &'static str {
        match self {
            Self::BypassFuel => "oreslang_rt_scalar_frame_step(a, 0) == 1",
            Self::DoubleCommit | Self::DropCommit => "a->counter == 1 && a->state == 1",
            Self::ForgeCancel => "oreslang_rt_scalar_frame_step(cancelled, 512) == 2",
        }
    }
    fn apply(self, ir: &str) -> Result<String, String> {
        match self {
            Self::BypassFuel => replace_line(ir, "%dispatch.batch.stop = select ", |line| {
                let (lhs, _) = line.split_once(" = select ").ok_or("missing select")?;
                Ok(format!("{lhs} = add i64 0, 1000"))
            }, self.id()),
            Self::DoubleCommit => replace_line(ir, "%counter.committed = add ", |line| {
                ensure(line.contains("add nsw i64 ") || line.contains("add i64 "),
                    "unexpected LLVM integer increment")?;
                let prefix = line.strip_suffix(", 1").ok_or("increment is not +1")?;
                Ok(format!("{prefix}, 2"))
            }, self.id()),
            Self::DropCommit => {
                // On #1569 the body reloads state on each iteration. Also
                // jump to the batch exit, or a stale-store mutant can hang.
                let lines: Vec<&str> = ir.lines().collect();
                let pairs: Vec<usize> = lines.iter().enumerate().filter_map(|(i, line)| {
                    if line.trim_start().starts_with("store i64 %counter.committed,")
                        && lines.get(i + 1).map(|x| x.trim() == "br label %loop.check").unwrap_or(false)
                    { Some(i) } else { None }
                }).collect();
                ensure(pairs.len() == 1,
                    format!("drop-commit: expected one store+branch, found {}", pairs.len()))?;
                let i = pairs[0];
                let mut out: Vec<String> = lines.iter().map(|x| x.to_string()).collect();
                out[i] = unique_replace(lines[i], "%counter.committed", "%counter.before", self.id())?;
                out[i + 1] = unique_replace(lines[i + 1], "%loop.check", "%loop.batch.exit", self.id())?;
                Ok(out.join("\n") + if ir.ends_with('\n') { "\n" } else { "" })
            }
            Self::ForgeCancel => replace_line(ir, "ret i32 2", |line| {
                ensure(line.trim() == "ret i32 2", "cancel status target isn't a return")?;
                Ok(line.replacen("ret i32 2", "ret i32 0", 1))
            }, self.id()),
        }
    }
}

struct Args { compiler: PathBuf, clang: PathBuf, fixture: PathBuf, harness: PathBuf }
impl Args {
    fn parse() -> Result<Self, String> {
        let (mut compiler, mut clang, mut fixture, mut harness) = (None, None, None, None);
        let mut input = env::args().skip(1);
        while let Some(key) = input.next() {
            let value = PathBuf::from(input.next().ok_or_else(|| format!("missing {key} value"))?);
            match key.as_str() {
                "--compiler" if compiler.is_none() => compiler = Some(value),
                "--clang" if clang.is_none() => clang = Some(value),
                "--fixture" if fixture.is_none() => fixture = Some(value),
                "--harness" if harness.is_none() => harness = Some(value),
                _ => return Err(format!("unknown or duplicate argument: {key}")),
            }
        }
        Ok(Self {
            compiler: compiler.ok_or("missing --compiler")?,
            clang: clang.ok_or("missing --clang")?,
            fixture: fixture.ok_or("missing --fixture")?,
            harness: harness.ok_or("missing --harness")?,
        })
    }
}
fn compile(args: &Args, work: &Path, source: &Path, gated: bool, id: &str)
    -> Result<Output, String>
{
    let mut command = Command::new(&args.compiler);
    if gated { command.arg("--experimental-rt-scalar-loop"); }
    run(command.arg(source), work, id)
}
fn link(args: &Args, work: &Path, ir: &Path, binary: &Path, optimization: &str, id: &str)
    -> Result<(), String>
{
    let mut command = Command::new(&args.clang);
    let output = run(command.args([
        "-std=c11", optimization, "-DNDEBUG", "-Wall", "-Wextra",
        "-Werror", "-Wno-error=override-module", "-pedantic",
    ]).arg(&args.harness).arg("-x").arg("ir").arg(ir).arg("-o").arg(binary), work, id)?;
    check_success(&output, id)
}

fn verify() -> Result<(), String> {
    let args = Args::parse()?;
    let workspace = Workspace::new()?;
    let work = workspace.path();
    let source = fs::read_to_string(&args.fixture).map_err(|e| format!("fixture: {e}"))?;

    let denied = compile(&args, work, &args.fixture, false, "default-deny")?;
    ensure(!denied.ok && denied.stdout.trim().is_empty(),
        "default source-loop admission reopened")?;
    ensure(denied.stderr.contains("requires verified LLVM continuation lowering"),
        "missing compiler default-deny diagnostic")?;
    let compiled = compile(&args, work, &args.fixture, true, "compiler-guest")?;
    check_success(&compiled, "real guest IR compiler")?;
    check_tokens(&compiled.stdout, &[
        "define i32 @oreslang_rt_scalar_frame_step",
        "define void @oreslang_rt_scalar_frame_init",
        "define void @oreslang_rt_scalar_frame_cancel",
        "define i64 @oreslang_rt_scalar_frame_result",
        "dispatch.batch.stop", "loop.batch.exit", "counter.valid", "frame.state.consistent",
    ], "real guest LLVM IR")?;
    ensure(!compiled.stdout.contains("fuel.remaining") && !compiled.stdout.contains("loop.fuel"),
        "per-backedge fuel check regressed")?;

    let ir = work.join("guest.ll");
    fs::write(&ir, &compiled.stdout).map_err(|e| format!("write IR: {e}"))?;
    let receipts = [
        "SCALAR_FRAME_AOT_TWO_TASKS_PASS",
        "SCALAR_FRAME_AOT_INVALID_FRAME_TRAPS_16_PASS",
        "SCALAR_FRAME_AOT_THREE_PEERS_BOUNDED_PROGRESS_PASS",
        "SCALAR_FRAME_AOT_CANCELLED_PEER_ISOLATION_PASS",
        "SCALAR_FRAME_AOT_BATCH_FUEL_BOUNDARIES_PASS",
    ];
    for opt in ["-O0", "-O2", "-O3"] {
        let id = format!("guest-{}", &opt[1..]);
        let binary = work.join(&id);
        link(&args, work, &ir, &binary, opt, &format!("{id}-build"))?;
        let outcome = run(&mut Command::new(&binary), work, &format!("{id}-run"))?;
        check_success(&outcome, &id)?;
        check_tokens(&outcome.stdout, &receipts, &id)?;
    }

    for mutation in Mutation::ALL {
        let name = mutation.id();
        let changed = mutation.apply(&compiled.stdout)?;
        let mutated_ir = work.join(format!("{name}.ll"));
        let binary = work.join(format!("{name}.exe"));
        fs::write(&mutated_ir, changed).map_err(|e| format!("{name} write: {e}"))?;
        // A mutant that fails to compile MUST NOT count as a killed mutant.
        link(&args, work, &mutated_ir, &binary, "-O2", &format!("{name}-build"))?;
        let outcome = run(&mut Command::new(&binary), work, &format!("{name}-run"))?;
        ensure(!outcome.ok, format!("{name}: mutant unexpectedly passed"))?;
        ensure(outcome.stderr.contains("frame check failed at ")
            && outcome.stderr.contains(mutation.expected()),
            format!("{name}: failure was not the expected native invariant: {}", outcome.stderr))?;
        ensure(!outcome.stdout.contains("SCALAR_FRAME_AOT_TWO_TASKS_PASS"),
            format!("{name}: mutant printed acceptance receipt"))?;
    }
    println!("SCALAR_FRAME_AOT_FOUR_REAL_IR_MUTATIONS_REJECTED");

    let invalids = [
        ("val count: int = 0;", "val count: int = -1;"),
        ("count = count + 1;", "count = count + 2;"),
        ("while (count < 1000)", "while (count < 2000000000)"),
        ("return count;", "return 17;"),
        ("val count: int = 0;", "const count: int = 0;"),
        ("count = count + 1;", "other = other + 1;"),
        ("while (count < 1000)",
         "while (count < 1000) { count = count + 1; } while (count < 2000)"),
    ];
    for (i, (before, after)) in invalids.iter().enumerate() {
        let source_path = work.join(format!("invalid-{i}.ores"));
        let negative = unique_replace(&source, before, after, &format!("invalid fixture {i}"))?;
        fs::write(&source_path, negative).map_err(|e| format!("negative fixture {i}: {e}"))?;
        let result = compile(&args, work, &source_path, true, &format!("negative-{i}"))?;
        ensure(!result.ok && result.stdout.trim().is_empty(),
            format!("invalid fixture {i} was accepted"))?;
    }
    let no_wasm = run(Command::new(&args.compiler)
        .arg("--experimental-rt-scalar-loop").arg("--target=wasm32").arg(&args.fixture),
        work, "forbidden-wasm32")?;
    ensure(!no_wasm.ok && no_wasm.stdout.trim().is_empty(),
        "wasm32 scalar frame target was incorrectly accepted")?;
    println!("NATIVE_OPT_IN_SCALAR_FRAME_PASS: real compiled Rust-driven guest O0/O2/O3, 16 traps, 4 compiled IR mutants, fuel, cancellation, peer, and negative-source cases");
    Ok(())
}
fn main() {
    if let Err(err) = verify() {
        eprintln!("SCALAR_FRAME_AOT_RUST_FAIL: {err}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    fn example() -> String {
        [
            "define i32 @step() {",
            "loop.check:",
            "  %dispatch.batch.stop = select i1 %cmp, i64 %end, i64 1000",
            "  %counter.before = load i64, ptr %frame",
            "  br label %loop.commit",
            "loop.commit:",
            "  %counter.committed = add nsw i64 %counter.before, 1",
            "  store i64 %counter.committed, ptr %frame, align 8",
            "  br label %loop.check",
            "cancelled:", "  ret i32 2", "}", "",
        ].join("\n")
    }
    #[test]
    fn mutants_have_unique_independent_ir_targets() {
        let ir = example();
        for mutation in Mutation::ALL {
            let changed = mutation.apply(&ir).unwrap();
            assert_ne!(changed, ir, "{mutation:?}");
            assert!(!changed.contains('\0'));
        }
        assert!(Mutation::BypassFuel.apply(&ir).unwrap().contains("stop = add i64 0, 1000"));
        assert!(Mutation::DoubleCommit.apply(&ir).unwrap().contains("before, 2"));
        assert!(Mutation::DropCommit.apply(&ir).unwrap().contains("store i64 %counter.before,"));
        assert!(Mutation::ForgeCancel.apply(&ir).unwrap().contains("ret i32 0"));
    }
    #[test]
    fn duplicate_targets_fail_closed() {
        let ir = example();
        let duplicates = [
            "  %dispatch.batch.stop = select i1 %cmp, i64 %end, i64 1000\n",
            "  %counter.committed = add nsw i64 %counter.before, 1\n",
            "  store i64 %counter.committed, ptr %frame, align 8\n  br label %loop.check\n",
            "  ret i32 2\n",
        ];
        for (i, mutation) in Mutation::ALL.iter().enumerate() {
            assert!(mutation.apply(&(ir.clone() + duplicates[i])).is_err(), "{mutation:?}");
        }
    }
    #[test]
    fn missing_targets_fail_closed() {
        for mutation in Mutation::ALL {
            assert!(mutation.apply("define void @foo() { ret void }").is_err());
        }
    }
    #[test]
    fn negative_fixture_substitution_cannot_be_ambiguous() {
        assert_eq!(unique_replace("a b", "a", "c", "case").unwrap(), "c b");
        assert!(unique_replace("a a", "a", "c", "case").is_err());
        assert!(unique_replace("a b", "missing", "c", "case").is_err());
    }
    #[test]
    fn cancellation_mutant_must_fail_cancellation_oracle() {
        assert_eq!(Mutation::ForgeCancel.expected(),
            "oreslang_rt_scalar_frame_step(cancelled, 512) == 2");
    }
}
