#[cfg(unix)]
use super::{require, run, stderr, stdout, TempDir};
use std::path::Path;

#[cfg(unix)]
fn expected_llvm_trap_signal() -> Result<i32, String> {
    // LLVM lowers llvm.trap to UD2/SIGILL on x86 and BRK/SIGTRAP on AArch64.
    // Keep this mapping explicit and fail closed on architectures we have not
    // qualified instead of accepting an unrelated fatal signal.
    #[cfg(any(target_arch = "x86", target_arch = "x86_64"))]
    {
        return Ok(4);
    }
    #[cfg(target_arch = "aarch64")]
    {
        return Ok(5);
    }
    #[allow(unreachable_code)]
    Err(format!(
        "checked-i64 LLVM trap signal is not pinned for architecture {}",
        std::env::consts::ARCH
    ))
}

// Fail closed if a compiled guest hangs. The guest exit status is the entire
// checked-i64 oracle; inherited pipes are unnecessary and could otherwise
// deadlock when a broken guest writes more than the pipe buffer.
#[cfg(unix)]
fn run_bounded_guest(
    binary: &Path,
    cwd: &Path,
    timeout: std::time::Duration,
) -> Result<std::process::ExitStatus, String> {
    use std::{
        process::{Command, Stdio},
        thread,
        time::{Duration, Instant},
    };
    let mut child = Command::new(binary)
        .current_dir(cwd)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .map_err(|e| format!("spawn native guest {}: {e}", binary.display()))?;
    let started = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(status)) => return Ok(status),
            Ok(None) if started.elapsed() < timeout => {
                thread::sleep(Duration::from_millis(10));
            }
            Ok(None) => {
                let _ = child.kill();
                let _ = child.wait(); // reap the child before failing the test
                return Err(format!(
                    "native guest {} exceeded {}s timeout",
                    binary.display(),
                    timeout.as_secs()
                ));
            }
            Err(e) => {
                let _ = child.kill();
                let _ = child.wait();
                return Err(format!("poll native guest {}: {e}", binary.display()));
            }
        }
    }
}

// These source-free unit tests do not require an Oreslang compiler or Clang.
// They prove both exit-status preservation and the bounded kill/reap path.
#[cfg(all(test, unix))]
mod bounded_guest_tests {
    use super::run_bounded_guest;
    use crate::TempDir;
    use std::{
        fs,
        os::unix::fs::PermissionsExt,
        path::{Path, PathBuf},
        time::Duration,
    };

    fn executable_script(root: &Path, name: &str, source: &str) -> PathBuf {
        let path = root.join(name);
        fs::write(&path, source).expect("write synthetic guest");
        let mut permissions = fs::metadata(&path).expect("guest metadata").permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&path, permissions).expect("make guest executable");
        path
    }

    #[test]
    fn preserves_normal_exit_status() {
        let temp = TempDir::new("ores-bounded-guest-exit-").unwrap();
        let guest = executable_script(&temp.path, "exit.sh", "#!/bin/sh\nexit 42\n");
        let status = run_bounded_guest(&guest, &temp.path, Duration::from_secs(5))
            .expect("guest should exit normally");
        assert_eq!(status.code(), Some(42));
    }

    #[test]
    fn terminates_nonexiting_guest() {
        let temp = TempDir::new("ores-bounded-guest-timeout-").unwrap();
        let guest = executable_script(
            &temp.path, "spin.sh", "#!/bin/sh\nwhile :; do :; done\n");
        let error = run_bounded_guest(&guest, &temp.path, Duration::from_millis(100))
            .expect_err("nonexiting guest must time out");
        assert!(error.contains("timeout"), "{error}");
    }
}

/// Execute compiler-produced native code, including genuine signal traps.
/// This is not a host arithmetic simulation or a typed actor-error contract.
#[cfg(unix)]
pub fn run_cases(compiler: &Path, clang: &Path) -> Result<(), String> {
    const SHARED_MATH_OK: &str =
        include_str!("../../fixtures/numeric-v1/math-i64-ok.ores");
    const SHARED_MATH_ABS_MIN: &str =
        include_str!("../../fixtures/numeric-v1/math-i64-abs-min.ores");
    const SHARED_MATH_NEG_MIN: &str =
        include_str!("../../fixtures/numeric-v1/math-i64-neg-min.ores");
    const SHARED_I64_ADD_OVERFLOW: &str =
        include_str!("../../fixtures/numeric-v1/i64-add-overflow.ores");
    const SHARED_I64_SUB_OVERFLOW: &str =
        include_str!("../../fixtures/numeric-v1/i64-sub-overflow.ores");
    const SHARED_I64_MUL_OVERFLOW: &str =
        include_str!("../../fixtures/numeric-v1/i64-mul-overflow.ores");
    const SHARED_I64_NEG_OVERFLOW: &str =
        include_str!("../../fixtures/numeric-v1/i64-neg-overflow.ores");
    const SHARED_I64_OVERFLOW_CATCH: &str =
        include_str!("../../fixtures/numeric-v1/i64-overflow-catch.ores");
    const SHARED_I64_OVERFLOW_TYPED_CATCH: &str =
        include_str!("../../fixtures/numeric-v1/i64-overflow-typed-catch.ores");
    const SHARED_I64_OVERFLOW_CROSS_FRAME_CATCH: &str =
        include_str!("../../fixtures/numeric-v1/i64-overflow-cross-frame-catch.ores");
    use std::{ffi::OsStr, fs, os::unix::process::ExitStatusExt};
    let temp = TempDir::new("ores-checked-i64-").map_err(|e| e.to_string())?;
    fs::write(temp.path.join("math-shadow.ores"),
        "define module Math as pub fnc abs(int value):int{return 7;} end\n")
        .map_err(|e| e.to_string())?;
    let positives = [
        ("precedence", "pub fnc main():int{return 2+3*4;}", 14),
        ("parentheses", "pub fnc main():int{return (2+3)*4;}", 20),
        ("signed", "pub fnc main():int{return -7*-6;}", 42),
        ("near-max", "pub fnc main():int{return 9223372036854775807-9223372036854775806;}", 1),
        ("near-min", "pub fnc main():int{return (-9223372036854775808+1)-(-9223372036854775808);}", 1),
        ("negate", "pub fnc main():int{return -(-42);}", 42),
        ("portable-math-i64-ok", SHARED_MATH_OK, 84),
        ("portable-overflow-catch", SHARED_I64_OVERFLOW_CATCH, 42),
        ("portable-overflow-cross-frame-catch", SHARED_I64_OVERFLOW_CROSS_FRAME_CATCH, 42),
        ("math-abs", "pub fnc main():int{return Math.abs(-42);}", 42),
        ("math-neg", "pub fnc main():int{return Math.neg(-42);}", 42),
        ("dynamic-math-abs", "fnc magnitude(int a):int{return Math.abs(a);} pub fnc main():int{return magnitude(-42);}", 42),
        // Boundary controls must return normally; checked negation in abs
        // may not spuriously trap on zero or the largest positive signed i64.
        ("math-abs-zero", "pub fnc main():int{return Math.abs(0)+42;}", 42),
        ("math-abs-max", "pub fnc main():int{return Math.abs(9223372036854775807)-9223372036854775807+42;}", 42),
        ("math-neg-zero", "pub fnc main():int{return Math.neg(0)+42;}", 42),
        ("math-neg-max", "pub fnc main():int{return Math.neg(9223372036854775807)+9223372036854775807+42;}", 42),
        ("imported-math-shadows-intrinsic",
         "import module Math from \"./math-shadow.ores\"; pub fnc main():int{return Math.abs(-42);}", 7),
        ("lazy-ternary", "pub fnc main():int{return false ? (9223372036854775807+1) : 42;}", 42),
        ("lazy-and", "pub fnc main():int{return (false && (9223372036854775807+1>0)) ? 0 : 42;}", 42),
        ("lazy-or", "pub fnc main():int{return (true || (9223372036854775807+1>0)) ? 42 : 0;}", 42),
        ("dynamic", "fnc add(int a,int b):int{return a+b;} pub fnc main():int{return add(40,2);}", 42),
    ];
    let rejects = [
        ("bigint-type", "pub fnc main(bigint x):int{return 0;}"),
        ("decimal-type", "pub fnc main(decimal x):int{return 0;}"),
        ("f32-type", "pub fnc main(f32 x):int{return 0;}"),
        ("f64-type", "pub fnc main(f64 x):int{return 0;}"),
        ("float-type", "pub fnc main(float x):int{return 0;}"),
        // Intrinsic admission must fail closed on wrong types, arity and
        // unknown names, without emitting even partial native LLVM IR.
        ("math-abs-bool", "pub fnc main():int{return Math.abs(true);}"),
        ("math-neg-bool", "pub fnc main():int{return Math.neg(false);}"),
        ("math-abs-no-arg", "pub fnc main():int{return Math.abs();}"),
        ("math-neg-no-arg", "pub fnc main():int{return Math.neg();}"),
        ("math-abs-two-args", "pub fnc main():int{return Math.abs(1,2);}"),
        ("math-neg-two-args", "pub fnc main():int{return Math.neg(1,2);}"),
        ("math-unknown-intrinsic", "pub fnc main():int{return Math.pow(2);}"),
        ("portable-overflow-typed-catch", SHARED_I64_OVERFLOW_TYPED_CATCH),
        ("overflow-catch-unproven-call",
         "fnc safe():int{return 42;} pub fnc main():int{try{return safe();}catch(err){return 1;}finally{}}"),
    ];
    for (name, source) in rejects {
        let input = temp.path.join(format!("{name}-reject.ores"));
        fs::write(&input, source).map_err(|e| e.to_string())?;
        let compiled = run(compiler, [input.as_os_str()])?;
        require(!compiled.status.success(), format!("{name}: unsupported numeric type compiled"))?;
        require(compiled.stdout.is_empty(), format!("{name}: rejected source emitted LLVM IR"))?;
        require(!stderr(&compiled).trim().is_empty(),
            format!("{name}: unsupported numeric type failed without a diagnostic"))?;
    }

    let negatives = [
        ("add", SHARED_I64_ADD_OVERFLOW, "llvm.sadd.with.overflow.i64"),
        ("subtract", SHARED_I64_SUB_OVERFLOW, "llvm.ssub.with.overflow.i64"),
        ("multiply", SHARED_I64_MUL_OVERFLOW, "llvm.smul.with.overflow.i64"),
        ("negate-min", SHARED_I64_NEG_OVERFLOW, "llvm.ssub.with.overflow.i64"),
        ("portable-math-abs-min", SHARED_MATH_ABS_MIN, "llvm.ssub.with.overflow.i64"),
        ("portable-math-neg-min", SHARED_MATH_NEG_MIN, "llvm.ssub.with.overflow.i64"),
        ("math-abs-min", "pub fnc main():int{return Math.abs(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
        ("math-neg-min", "pub fnc main():int{return Math.neg(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-math-abs-min", "fnc magnitude(int a):int{return Math.abs(a);} pub fnc main():int{return magnitude(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-add", "fnc add(int a,int b):int{return a+b;} pub fnc main():int{return add(9223372036854775807,1);}", "llvm.sadd.with.overflow.i64"),
        ("dynamic-subtract", "fnc sub(int a,int b):int{return a-b;} pub fnc main():int{return sub(-9223372036854775808,1);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-multiply", "fnc mul(int a,int b):int{return a*b;} pub fnc main():int{return mul(3037000500,3037000500);}", "llvm.smul.with.overflow.i64"),
        ("dynamic-negate", "fnc neg(int a):int{return -a;} pub fnc main():int{return neg(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-cross-frame-uncaught",
         "fnc leaf():int{return 9223372036854775807+1;} fnc middle():int{return leaf();} pub fnc main():int{return middle();}",
         "llvm.sadd.with.overflow.i64"),
    ];
    let mut executions = 0;
    for optimization in ["-O1", "-O2"] {
        for (name, source, expected, intrinsic) in positives.iter().map(|(n,s,e)| (*n,*s,Some(*e),None))
            .chain(negatives.iter().map(|(n,s,i)| (*n,*s,None,Some(*i)))) {
            let input = temp.path.join(format!("{name}.ores"));
            let ir = temp.path.join(format!("{name}.ll"));
            let binary = temp.path.join(format!("{name}-{}", &optimization[1..]));
            fs::write(&input, source).map_err(|e| e.to_string())?;
            let compiled = run(compiler, [input.as_os_str()])?;
            require(compiled.status.success(), format!("{name}: {}", stderr(&compiled)))?;
            if let Some(intrinsic) = intrinsic {
                require(stdout(&compiled).contains(intrinsic), format!("{name}: missing checked intrinsic"))?;
            }
            if name == "portable-overflow-catch" {
                let emitted = stdout(&compiled);
                require(emitted.contains("throw.catch.numeric"),
                    format!("{name}: missing numeric catch CFG"))?;
                require(emitted.contains("65537"),
                    format!("{name}: missing ArithmeticOverflowError ABI code"))?;
                require(emitted.contains("numeric.effect.kind"),
                    format!("{name}: missing typed Throw discriminant slot"))?;
            }
            if name == "portable-overflow-cross-frame-catch" {
                let emitted = stdout(&compiled);
                require(emitted.contains("{ i64, i64, i64 } @leaf("),
                    format!("{name}: leaf did not adopt effect-result ABI"))?;
                require(emitted.contains("{ i64, i64, i64 } @middle("),
                    format!("{name}: middle did not adopt transitive effect-result ABI"))?;
                require(emitted.contains("call.effect.propagate"),
                    format!("{name}: missing cross-frame propagation CFG"))?;
                require(emitted.contains("throw.catch.numeric"),
                    format!("{name}: outer catch CFG missing"))?;
                require(emitted.contains("65537"),
                    format!("{name}: missing ArithmeticOverflowError ABI code"))?;

                for helper in ["leaf", "middle"] {
                    let marker = format!("define {{ i64, i64, i64 }} @{helper}(");
                    let start = emitted.find(&marker)
                        .ok_or_else(|| format!("{name}: missing {helper} definition"))?;
                    let tail = &emitted[start..];
                    let end = tail.find("\n}")
                        .ok_or_else(|| format!("{name}: unterminated {helper} definition"))?;
                    require(!tail[..end].contains("llvm.trap"),
                        format!("{name}: {helper} still traps instead of forwarding effect"))?;
                }
            }
            fs::write(&ir, &compiled.stdout).map_err(|e| e.to_string())?;
            let linked = run(clang, [OsStr::new("-x"), OsStr::new("ir"), OsStr::new(optimization),
                ir.as_os_str(), OsStr::new("-o"), binary.as_os_str()])?;
            require(linked.status.success(), format!("{name}: {}", stderr(&linked)))?;
            let observed = run_bounded_guest(
                &binary, &temp.path, std::time::Duration::from_secs(30))
                .map_err(|e| format!("{name} {optimization}: {e}"))?;
            if let Some(expected) = expected {
                require(observed.code() == Some(expected),
                    format!("{name} {optimization}: expected {expected}, got {:?}", observed))?;
            } else {
                // Match the target's precise LLVM trap signal; normal exits,
                // unrelated fatal signals, and unqualified architectures fail.
                let signal = expected_llvm_trap_signal()?;
                require(observed.signal() == Some(signal),
                    format!("{name} {optimization}: expected trap signal {signal}, got {:?}", observed))?;
            }
            executions += 1;
        }
    }
    println!("PASS {executions} actual native checked-i64 executions, O1/O2; no typed actor-error claim");
    Ok(())
}

#[cfg(not(unix))]
pub fn run_cases(_compiler: &Path, _clang: &Path) -> Result<(), String> {
    Err("checked-i64 signal regression requires Unix".into())
}
