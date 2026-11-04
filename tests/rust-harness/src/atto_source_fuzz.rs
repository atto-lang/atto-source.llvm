//! Compiler-backed, deterministic AttoLang source mutation smoke for LLVM.
//! This is not stdin/coverage fuzzing or a trusted live actor race detector.
use super::{require, run, stderr, stdout, TempDir};
use std::ffi::OsStr;
use std::fs;
use std::path::Path;

const MAX_CASES: usize = 24;

struct SplitMix64 {
    state: u64,
}
impl SplitMix64 {
    fn new(seed: u64) -> Self {
        Self { state: seed }
    }
    fn next(&mut self) -> u64 {
        self.state = self.state.wrapping_add(0x9e3779b97f4a7c15);
        let mut z = self.state;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d049bb133111eb);
        z ^ (z >> 31)
    }
}

fn operands(random: &mut SplitMix64) -> (u32, u32) {
    ((random.next() % 97) as u32, (random.next() % 97) as u32)
}
fn guest(a: u32, b: u32) -> String {
    format!(
        "pub fnc main(): int {{ return add({a}, {b}); }}\n\
         fnc add(int x, int y): int {{ return x + y; }}\n"
    )
}
fn check_exit(observed: Option<i32>, expected: i32) -> Result<(), String> {
    require(
        observed == Some(expected),
        format!("LLVM native guest exit {observed:?} != oracle {expected}"),
    )
}

fn compile_and_run(
    compiler: &Path,
    clang: &Path,
    source: &Path,
    ir: &Path,
    executable: &Path,
    expected: i32,
    level: &str,
) -> Result<(), String> {
    let compiled = run(
        compiler,
        [OsStr::new("--target=host"), source.as_os_str()],
    )?;
    require(
        compiled.status.success(),
        format!("LLVM .atto source compilation failed: {}", stderr(&compiled)),
    )?;
    let text = stdout(&compiled);
    require(
        compiled.stdout.len() <= 2_000_000
            && compiled.stderr.is_empty()
            && text.starts_with("; ModuleID")
            && text.contains("define i32 @main("),
        "invalid, excessive, or incomplete LLVM guest IR",
    )?;
    fs::write(ir, &compiled.stdout).map_err(|e| e.to_string())?;
    let linked = run(
        clang,
        [
            OsStr::new(level),
            ir.as_os_str(),
            OsStr::new("-o"),
            executable.as_os_str(),
        ],
    )?;
    require(
        linked.status.success() && linked.stderr.len() <= 16_384,
        format!("Clang link failed: {}", stderr(&linked)),
    )?;
    let executed = std::process::Command::new(executable)
        .env_clear()
        .stdin(std::process::Stdio::null())
        .output()
        .map_err(|e| format!("native guest failed to start: {e}"))?;
    require(
        executed.stdout.is_empty() && executed.stderr.is_empty(),
        "unexpected native guest stdout/stderr; refusing an unscoped test receipt",
    )?;
    check_exit(executed.status.code(), expected)
}

pub(super) fn run_cases(
    compiler: &Path,
    clang: &Path,
    seed: u64,
    cases: usize,
) -> Result<(), String> {
    require(
        (1..=MAX_CASES).contains(&cases),
        "atto-source-fuzz case count must be 1..24",
    )?;
    require(
        compiler.is_file() && clang.is_file(),
        "LLVM compiler and Clang must be existing executable paths",
    )?;
    let temp = TempDir::new("atto-llvm-source-fuzz-").map_err(|e| e.to_string())?;
    let src = temp.path.join("case.atto");
    let legacy = temp.path.join("case.ores");
    let ir = temp.path.join("source.ll");
    let exe = temp.path.join("guest");
    let mut random = SplitMix64::new(seed);
    let mut compiled_runs = 0usize;

    // Same source, different suffixes, and both native optimization levels.
    fs::write(&src, guest(40, 2)).map_err(|e| e.to_string())?;
    fs::write(&legacy, guest(40, 2)).map_err(|e| e.to_string())?;
    for source in [&src, &legacy] {
        for level in ["-O0", "-O2"] {
            compile_and_run(compiler, clang, source, &ir, &exe, 42, level)?;
            compiled_runs += 1;
        }
    }
    for index in 0..cases {
        let (a, b) = operands(&mut random);
        let expected = (a + b) as i32;
        fs::write(&src, guest(a, b)).map_err(|e| e.to_string())?;
        for level in ["-O0", "-O2"] {
            compile_and_run(compiler, clang, &src, &ir, &exe, expected, level)
                .map_err(|e| format!("seed={seed} case={index} a={a} b={b} {level}: {e}"))?;
            compiled_runs += 1;
        }
    }
    // An altered guest result MUST fail the independent output oracle.
    fs::write(&src, guest(40, 3)).map_err(|e| e.to_string())?;
    let altered = run(compiler, [OsStr::new("--target=host"), src.as_os_str()])?;
    require(
        altered.status.success()
            && altered.stdout.len() <= 2_000_000
            && altered.stderr.is_empty(),
        "mutated guest must compile before the negative execution oracle",
    )?;
    fs::write(&ir, altered.stdout).map_err(|e| e.to_string())?;
    let linked = run(
        clang,
        [OsStr::new("-O0"), ir.as_os_str(), OsStr::new("-o"), exe.as_os_str()],
    )?;
    require(linked.status.success(), "mutated guest link must execute")?;
    let mutated = run(&exe, std::iter::empty::<&str>())?;
    require(
        mutated.stdout.is_empty()
            && mutated.stderr.is_empty()
            && mutated.status.code() == Some(43)
            && check_exit(mutated.status.code(), 42).is_err(),
        "negative guest-output mutation failed to expose oracle mismatch",
    )?;

    // Malformed or unsupported source cannot emit even a partial LLVM module.
    for (label, invalid) in [
        ("unknown-call", "pub fnc main(): int { return unknown(7); }\n"),
        ("incomplete-return", "pub fnc main(): int { return 3 + ; }\n"),
    ] {
        fs::write(&src, invalid).map_err(|e| e.to_string())?;
        let rejected = run(compiler, [OsStr::new("--target=host"), src.as_os_str()])?;
        require(
            !rejected.status.success()
                && rejected.stdout.is_empty()
                && !rejected.stderr.is_empty()
                && rejected.stderr.len() < 16_384,
            format!("{label}: native parser/compiler did not fail closed"),
        )?;
    }
    println!(
        "ATTO_LLVM_COMPILED_SOURCE_FUZZ_PASS seed={seed} cases={cases} native_executions={compiled_runs} negative_oracles=3 extension_parity=atto,ores"
    );
    Ok(())
}

pub(super) fn self_test() -> Result<(), String> {
    let mut a = SplitMix64::new(1729);
    let mut b = SplitMix64::new(1729);
    for _ in 0..1000 {
        require(a.next() == b.next(), "seeded fuzzer reproducibility")?;
    }
    let mut rng = SplitMix64::new(0);
    for _ in 0..1000 {
        let (a, b) = operands(&mut rng);
        require(a <= 96 && b <= 96 && a + b <= 192, "guest oracle budget")?;
    }
    require(check_exit(Some(43), 42).is_err(), "mutation must be detected")?;
    require(check_exit(None, 42).is_err(), "signal/abnormal exit must fail")?;
    require(check_exit(Some(42), 42).is_ok(), "clean expected result")?;
    println!("ATTO_LLVM_SOURCE_FUZZ_HOST_ORACLE_PASS");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn seeded_cases_replay_identically_and_stay_within_exit_domain() {
        for seed in [0, 1, 42, 1729, u64::MAX] {
            let mut a = SplitMix64::new(seed);
            let mut b = SplitMix64::new(seed);
            for _ in 0..1000 {
                let left = operands(&mut a);
                let right = operands(&mut b);
                assert_eq!(left, right);
                assert!(left.0 + left.1 <= 192);
                let generated = guest(left.0, left.1);
                assert!(generated.contains("pub fnc main(): int"));
                assert!(generated.contains("return x + y"));
            }
        }
    }

    #[test]
    fn oracle_never_accepts_wrong_exit_or_signal() {
        assert!(check_exit(Some(42), 42).is_ok());
        for status in [None, Some(0), Some(41), Some(43), Some(255)] {
            assert!(check_exit(status, 42).is_err());
        }
    }

    #[test]
    fn source_fuzzer_rejects_zero_and_excessive_budget_before_compiler_access() {
        let missing = Path::new("/nonexistent-atto-compiler");
        assert!(run_cases(missing, missing, 1729, 0).is_err());
        assert!(run_cases(missing, missing, 1729, MAX_CASES + 1).is_err());
    }
}
