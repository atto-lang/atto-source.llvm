// Native source-to-IR-to-Clang execution, not a parser-only or model test.
// No new Python harnesses: this standalone executable uses Rust std only.
use std::fs;
use std::path::PathBuf;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct Workdir(PathBuf);
impl Workdir {
    fn new() -> Self {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH).expect("clock")
            .as_nanos();
        let path = std::env::temp_dir().join(format!(
            "oreslang-unary-plus-{}-{}", std::process::id(), nonce));
        fs::create_dir(&path).expect("create workdir");
        Self(path)
    }
}
impl Drop for Workdir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

fn compile(compiler: &str, dir: &Workdir, id: usize, source: &str) -> std::process::Output {
    let input = dir.0.join(format!("case-{id}.ores"));
    fs::write(&input, source).expect("write source");
    Command::new(compiler).arg(input).output().expect("run real Oreslang compiler")
}

fn main() {
    let mut args = std::env::args().skip(1);
    let compiler = args.next().expect("usage: native-unary-plus compiler clang");
    let clang = args.next().expect("missing clang");
    assert!(args.next().is_none(), "unexpected arguments");
    let dir = Workdir::new();

    let valid = [
        ("pub fnc main():int{return +42;}\n", 42),
        ("pub fnc main():int{return ++(40+2);}\n", 42),
        ("pub fnc main():int{return 2 * +(20+1);}\n", 42),
        ("pub fnc main():int{return -+(0-42);}\n", 42),
        ("pub fnc main():int{return +0;}\n", 0),
        ("pub fnc main():int{return +(4_0+2);}\n", 42),
        ("pub fnc main():int{return + (40_0 - 35_8);}\n", 42),
        // Exercise signed i64 endpoints inside the real guest: process
        // exit status alone truncates i64 and is not endpoint evidence.
        ("pub fnc main():int{if (-9_223_372_036_854_775_808 eq -0_9223372036854775808) {return 42;} fi return 1;}\n", 42),
        ("pub fnc main():int{if (0_9223372036854775807 eq 9_223_372_036_854_775_807) {return 42;} fi return 1;}\n", 42),
        ("pub fnc main():int{if (-00_9223372036854775808 eq -9223372036854775808) {return 42;} fi return 1;}\n", 42),
        ("pub fnc main():int{if (-0_0_9223372036854775808 eq -9_223_372_036_854_775_808) {return 42;} fi return 1;}\n", 42),
    ];
    for (i, (source, expected)) in valid.iter().enumerate() {
        let result = compile(&compiler, &dir, i, source);
        assert!(result.status.success(),
            "source case {i} rejected: {}", String::from_utf8_lossy(&result.stderr));
        let ir = dir.0.join(format!("case-{i}.ll"));
        assert!(result.stdout.windows(6).any(|bytes| bytes == b"define"),
            "case {i} did not emit LLVM function definitions");
        fs::write(&ir, result.stdout).expect("write LLVM IR");
        let executable = dir.0.join(format!("case-{i}.native"));
        let built = Command::new(&clang)
            .args(["-x", "ir"]).arg(&ir).arg("-o").arg(&executable)
            .output().expect("run clang");
        assert!(built.status.success(),
            "Clang could not build case {i}: {}", String::from_utf8_lossy(&built.stderr));
        let run = Command::new(executable).output().expect("execute AOT guest");
        assert_eq!(run.status.code(), Some(*expected), "native AOT case {i}");
    }

    // Arithmetic unary plus must not launder bool/String into i64, and no
    // malformed or out-of-range program may emit even partial LLVM IR.
    let invalid = vec![
        "pub fnc main():int{return +true;}\n".to_string(),
        "pub fnc main():int{return +\"x\";}\n".to_string(),
        "pub fnc main():int{return +9223372036854775808;}\n".to_string(),
        "pub fnc main():int{return +;}\n".to_string(),
        format!("pub fnc main():int{{return {}1;}}\n", "+".repeat(128)),
        "pub fnc main():int{return 1__0;}\n".to_string(),
        "pub fnc main():int{return 1_;}\n".to_string(),
        "pub fnc main():int{return 1_+2;}\n".to_string(),
        "pub fnc main():int{return 9_223_372_036_854_775_808;}\n".to_string(),
        "pub fnc main():int{return -0_9223372036854775809;}\n".to_string(),
        "pub fnc main():int{return -9_223_372_036_854_775_809;}\n".to_string(),
        "pub fnc main():int{return -0__9223372036854775808;}\n".to_string(),
    ];
    for (i, source) in invalid.iter().enumerate() {
        let result = compile(&compiler, &dir, valid.len() + i, source);
        assert!(!result.status.success(), "invalid unary plus case {i} compiled");
        assert!(result.stdout.is_empty(),
            "invalid unary plus case {i} leaked LLVM IR to stdout");
        assert!(!result.stderr.is_empty(),
            "invalid unary plus case {i} rejected without a diagnostic");
    }
    println!("eleven native executable unary-plus/bounds positives, twelve no-IR negatives");
}
