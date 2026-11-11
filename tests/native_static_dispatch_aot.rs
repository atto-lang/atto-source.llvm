//! Executable AttoLang class calls must lower to direct LLVM symbols in full AOT.
use std::collections::BTreeSet;

fn call_operands(ir: &str) -> Vec<String> {
    ir.lines()
        .map(str::trim)
        .filter(|line| {
            !line.starts_with(';')
                && (line.starts_with("call ")
                    || line.starts_with("invoke ")
                    || line.contains(" call ")
                    || line.contains(" invoke "))
        })
        .filter_map(|line| {
            let operand = line
                .split_once(" call ")
                .map(|(_, s)| s)
                .or_else(|| line.split_once(" invoke ").map(|(_, s)| s))
                .or_else(|| line.strip_prefix("call "))
                .or_else(|| line.strip_prefix("invoke "))?;
            Some(operand.split('(').next().unwrap_or(operand).trim().to_string())
        })
        .collect()
}

fn contains_method_identity(symbol: &str, target: &str) -> bool {
    symbol.match_indices(target).any(|(start, _)| {
        let bytes = symbol.as_bytes();
        let before_ok = start == 0
            || !bytes[start - 1].is_ascii_alphanumeric() && bytes[start - 1] != b'_';
        let after = start + target.len();
        let after_ok = after == symbol.len()
            || !bytes[after].is_ascii_alphanumeric() && bytes[after] != b'_';
        before_ok && after_ok
    })
}

fn require_direct_method_calls(ir: &str, targets: &[&str]) -> Result<(), String> {
    let calls = call_operands(ir);
    let mut found = BTreeSet::new();
    for operands in calls {
        // A typed direct callee contains @symbol or @"quoted.symbol".
        // A function-pointer operand without @ is a regression for this
        // deliberately non-polymorphic, closed-world test program.
        if let Some(at) = operands.rfind('@') {
            let symbol = operands[at + 1..].trim_matches('"');
            for target in targets {
                if contains_method_identity(symbol, target) {
                    found.insert(*target);
                }
            }
        } else if operands
            .split_whitespace()
            .last()
            .is_some_and(|token| token.starts_with('%'))
        {
            return Err(format!("unexpected indirect LLVM call: {operands}"));
        }
    }
    for target in targets {
        if !found.contains(target) {
            return Err(format!("missing emitted direct LLVM method call: {target}"));
        }
    }
    Ok(())
}

#[cfg(not(test))]
mod executable {
    use super::require_direct_method_calls;
    use std::{env, fs, path::{Path, PathBuf}, process::{self, Command}, time::{SystemTime, UNIX_EPOCH}};

    const POSITIVE: &str = r#"define class Counter as
  let int value=40;
  pub read():int{return self.value;}
  pub add(mut self)(int n):void{self.value=self.value+n;return;}
end
pub fnc main():int{
  let mut Counter c=new Counter();
  c.add(2);
  return c.read();
}
"#;

    struct Scratch(PathBuf);
    impl Scratch {
        fn new() -> Result<Self, String> {
            let nanos = SystemTime::now().duration_since(UNIX_EPOCH)
                .map_err(|err| err.to_string())?.as_nanos();
            for suffix in 0..32 {
                let path = env::temp_dir().join(format!(
                    "atto-static-dispatch-{}-{nanos}-{suffix}", process::id()));
                match fs::create_dir(&path) {
                    Ok(()) => return Ok(Self(path)),
                    Err(err) if err.kind() == std::io::ErrorKind::AlreadyExists => continue,
                    Err(err) => return Err(err.to_string()),
                }
            }
            Err("cannot create isolated dispatch test directory".into())
        }
    }
    impl Drop for Scratch { fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); } }

    fn compile_source(compiler: &Path, file: &Path) -> Result<std::process::Output, String> {
        Command::new(compiler).arg(file).output()
            .map_err(|err| format!("failed to launch native compiler: {err}"))
    }

    fn must_reject(compiler: &Path, root: &Path, name: &str, source: &str, reason: &str) -> Result<(), String> {
        let path = root.join(format!("{name}.atto"));
        fs::write(&path, source).map_err(|err| err.to_string())?;
        let output = compile_source(compiler, &path)?;
        let stderr = String::from_utf8_lossy(&output.stderr);
        if output.status.success() || !output.stdout.is_empty() || !stderr.contains(reason) {
            return Err(format!("{name}: should reject without emitting IR; code={:?}; stderr={stderr}", output.status.code()));
        }
        Ok(())
    }

    pub(super) fn run() -> Result<(), String> {
        let mut args = env::args_os().skip(1);
        let compiler = PathBuf::from(args.next().ok_or("usage: test COMPILER CLANG")?);
        let clang = PathBuf::from(args.next().ok_or("usage: test COMPILER CLANG")?);
        if args.next().is_some() { return Err("unexpected extra argument".into()); }
        let root = Scratch::new()?;
        let source = root.0.join("main.atto");
        fs::write(&source, POSITIVE).map_err(|err| err.to_string())?;
        let compiled = compile_source(&compiler, &source)?;
        if !compiled.status.success() {
            return Err(format!("compiler rejection: {}", String::from_utf8_lossy(&compiled.stderr)));
        }
        let ir = String::from_utf8(compiled.stdout)
            .map_err(|_| "compiler returned non-UTF8 LLVM IR".to_string())?;
        require_direct_method_calls(&ir, &["Counter.add", "Counter.read"])?;
        let llvm_path = root.0.join("main.ll");
        fs::write(&llvm_path, ir).map_err(|err| err.to_string())?;
        for opt in ["-O0", "-O2"] {
            let binary = root.0.join(if opt == "-O0" { "guest_o0" } else { "guest_o2" });
            let link = Command::new(&clang).args([opt, "-x", "ir"]).arg(&llvm_path)
                .arg("-o").arg(&binary).output()
                .map_err(|err| format!("could not launch clang: {err}"))?;
            if !link.status.success() {
                return Err(format!("clang {opt} failed: {}", String::from_utf8_lossy(&link.stderr)));
            }
            let executed = Command::new(&binary).output()
                .map_err(|err| format!("native guest did not execute: {err}"))?;
            if executed.status.code() != Some(42) {
                return Err(format!("{opt} guest expected exit 42, got {:?}: {}", executed.status.code(), String::from_utf8_lossy(&executed.stderr)));
            }
        }
        const PREFIX: &str = "define class Counter as let int value=40; pub read():int{return self.value;} end\n";
        must_reject(&compiler, &root.0, "wrong_arity",
            &(PREFIX.to_string() + "pub fnc main():int{let Counter c=new Counter();return c.read(1);}"),
            "explicit call arity")?;
        must_reject(&compiler, &root.0, "missing_method",
            &(PREFIX.to_string() + "pub fnc main():int{let Counter c=new Counter();return c.lookup();}"),
            "unknown method")?;
        must_reject(&compiler, &root.0, "duplicate_slot",
            "define class Counter as let int value=42; pub read():int{return self.value;} pub read():int{return 0;} end pub fnc main():int{return 0;}",
            "duplicate native method")?;
        eprintln!("PASS: direct symbol method calls, O0/O2 guest exit 42, and 3 source rejections");
        Ok(())
    }
}

#[cfg(not(test))]
fn main() {
    if let Err(err) = executable::run() {
        eprintln!("FAIL: {err}");
        std::process::exit(1);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn identifies_direct_calls_and_quoted_targets() {
        let ir = "%v = call i64 @\"Counter.read$1\"(ptr %c)\n  call void @\"Counter.add$2\"(ptr %c, i64 2)";
        assert!(require_direct_method_calls(ir, &["Counter.read", "Counter.add"]).is_ok());
    }
    #[test]
    fn rejects_indirect_function_pointer_call() {
        let ir = "%v = call i64 %fn(ptr %c)\n%w = call i64 @\"Counter.read\"(ptr %c)";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }
    #[test]
    fn rejects_missing_method_and_declaration_spoofing() {
        let ir = "define i64 @\"Counter.read\"(ptr %c) { ret i64 42 }";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }
    #[test]
    fn does_not_confuse_prefix_names() {
        let ir = "%v = call i64 @NotCounter.read(ptr %c)";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }
}
