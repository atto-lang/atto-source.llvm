#[cfg(unix)]
use super::{require, run, stderr, stdout, TempDir};
use std::path::Path;

/// Execute compiler-produced native code, including genuine signal traps.
/// This is not a host arithmetic simulation or a typed actor-error contract.
#[cfg(unix)]
pub fn run_cases(compiler: &Path, clang: &Path) -> Result<(), String> {
    use std::{ffi::OsStr, fs, os::unix::process::ExitStatusExt, process::Command};
    let temp = TempDir::new("ores-checked-i64-").map_err(|e| e.to_string())?;
    let positives = [
        ("precedence", "pub fnc main():int{return 2+3*4;}", 14),
        ("parentheses", "pub fnc main():int{return (2+3)*4;}", 20),
        ("signed", "pub fnc main():int{return -7*-6;}", 42),
        ("near-max", "pub fnc main():int{return 9223372036854775807-9223372036854775806;}", 1),
        ("near-min", "pub fnc main():int{return (-9223372036854775808+1)-(-9223372036854775808);}", 1),
        ("negate", "pub fnc main():int{return -(-42);}", 42),
        ("lazy-ternary", "pub fnc main():int{return false ? (9223372036854775807+1) : 42;}", 42),
        ("lazy-and", "pub fnc main():int{return (false && (9223372036854775807+1>0)) ? 0 : 42;}", 42),
        ("lazy-or", "pub fnc main():int{return (true || (9223372036854775807+1>0)) ? 42 : 0;}", 42),
        ("dynamic", "fnc add(int a,int b):int{return a+b;} pub fnc main():int{return add(40,2);}", 42),
    ];
    let negatives = [
        ("add", "pub fnc main():int{return 9223372036854775807+1;}", "llvm.sadd.with.overflow.i64"),
        ("subtract", "pub fnc main():int{return -9223372036854775808-1;}", "llvm.ssub.with.overflow.i64"),
        ("multiply", "pub fnc main():int{return 3037000500*3037000500;}", "llvm.smul.with.overflow.i64"),
        ("negate-min", "pub fnc main():int{return -(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-add", "fnc add(int a,int b):int{return a+b;} pub fnc main():int{return add(9223372036854775807,1);}", "llvm.sadd.with.overflow.i64"),
        ("dynamic-subtract", "fnc sub(int a,int b):int{return a-b;} pub fnc main():int{return sub(-9223372036854775808,1);}", "llvm.ssub.with.overflow.i64"),
        ("dynamic-multiply", "fnc mul(int a,int b):int{return a*b;} pub fnc main():int{return mul(3037000500,3037000500);}", "llvm.smul.with.overflow.i64"),
        ("dynamic-negate", "fnc neg(int a):int{return -a;} pub fnc main():int{return neg(-9223372036854775808);}", "llvm.ssub.with.overflow.i64"),
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
            fs::write(&ir, &compiled.stdout).map_err(|e| e.to_string())?;
            let linked = run(clang, [OsStr::new("-x"), OsStr::new("ir"), OsStr::new(optimization),
                ir.as_os_str(), OsStr::new("-o"), binary.as_os_str()])?;
            require(linked.status.success(), format!("{name}: {}", stderr(&linked)))?;
            let observed = Command::new(&binary).current_dir(&temp.path).output()
                .map_err(|e| e.to_string())?;
            if let Some(expected) = expected {
                require(observed.status.code() == Some(expected),
                    format!("{name} {optimization}: expected {expected}, got {:?}", observed.status))?;
            } else {
                // LLVM emits UD2/SIGILL on Linux x64 and BRK/SIGTRAP on
                // Apple Silicon. Match the target's precise trap signal;
                // normal exits and unrelated signals must still fail.
                let signal = if cfg!(all(target_os = "macos", target_arch = "aarch64")) { 5 } else { 4 };
                require(observed.status.signal() == Some(signal),
                    format!("{name} {optimization}: expected trap signal {signal}, got {:?}", observed.status))?;
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
