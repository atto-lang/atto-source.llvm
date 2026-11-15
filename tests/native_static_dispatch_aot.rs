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
            // The rightmost '(' starts call arguments; earlier parentheses
            // may describe a function-pointer type, e.g. i64 (ptr) %fp.
            Some(operand.rsplit_once('(').map(|(before, _)| before)
                .unwrap_or(operand).trim().to_string())
        })
        .collect()
}

fn require_direct_method_calls(ir: &str, targets: &[&str]) -> Result<(), String> {
    let calls = call_operands(ir);
    let mut found = BTreeSet::new();
    for operands in calls {
        // A typed direct callee contains @symbol or @"quoted.symbol".
        // A function-pointer operand without @ is a regression for this
        // deliberately non-polymorphic, closed-world test program.
        let token = operands.split_whitespace().last()
            .ok_or_else(|| format!("LLVM call has no callee: {operands}"))?;
        // Reject *every* unrecognized call form, not just %function-pointers:
        // bitcast/inline-asm/typed-indirect calls can otherwise coexist with
        // a genuine direct call and silently evade this AOT proof.
        let symbol = token.strip_prefix('@')
            .ok_or_else(|| format!("unverified indirect or computed LLVM call: {operands}"))?;
        // Match the *entire* compiler ABI symbol. Prefix, suffix,
        // namespace and arity lookalikes must not satisfy this gate.
        let symbol = symbol.trim_matches('"');
        if symbol.is_empty() {
            return Err(format!("empty LLVM call symbol: {operands}"));
        }
        if targets.contains(&symbol) {
            found.insert(symbol.to_string());
        }
    }
    for target in targets {
        if !found.contains(*target) {
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

    const OVERLOADED: &str = r#"define class Box as
  let int value=20;
  pub read():int{return self.value;}
  pub read(int n):int{return self.value+n;}
end
pub fnc main():int{
  let Box b=new Box();
  return b.read()+b.read(2);
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

    fn compile_and_execute(compiler: &Path, clang: &Path, root: &Path, stem: &str,
                           source: &str, expected_calls: &[&str]) -> Result<(), String> {
        let source_path = root.join(format!("{stem}.atto"));
        fs::write(&source_path, source).map_err(|err| err.to_string())?;
        let compiled = compile_source(compiler, &source_path)?;
        if !compiled.status.success() {
            return Err(format!("{stem}: compiler rejection: {}",
                String::from_utf8_lossy(&compiled.stderr)));
        }
        let ir = String::from_utf8(compiled.stdout)
            .map_err(|_| format!("{stem}: compiler returned non-UTF8 LLVM IR"))?;
        require_direct_method_calls(&ir, expected_calls)?;
        let llvm_path = root.join(format!("{stem}.ll"));
        fs::write(&llvm_path, ir).map_err(|err| err.to_string())?;
        for opt in ["-O0", "-O2"] {
            let binary = root.join(format!("{stem}_{}", if opt == "-O0" { "o0" } else { "o2" }));
            let link = Command::new(clang).args([opt, "-x", "ir"]).arg(&llvm_path)
                .arg("-o").arg(&binary).output()
                .map_err(|err| format!("{stem}: could not launch clang: {err}"))?;
            if !link.status.success() {
                return Err(format!("{stem}: clang {opt} failed: {}",
                    String::from_utf8_lossy(&link.stderr)));
            }
            let executed = Command::new(&binary).output()
                .map_err(|err| format!("{stem}: native guest did not execute: {err}"))?;
            if executed.status.code() != Some(42) {
                return Err(format!("{stem}: {opt} guest expected exit 42, got {:?}: {}",
                    executed.status.code(), String::from_utf8_lossy(&executed.stderr)));
            }
        }
        Ok(())
    }

    pub(super) fn run() -> Result<(), String> {
        let mut args = env::args_os().skip(1);
        let compiler = PathBuf::from(args.next().ok_or("usage: test COMPILER CLANG")?);
        let clang = PathBuf::from(args.next().ok_or("usage: test COMPILER CLANG")?);
        if args.next().is_some() { return Err("unexpected extra argument".into()); }
        let root = Scratch::new()?;
        compile_and_execute(&compiler, &clang, &root.0, "simple", POSITIVE,
            &["Counter.add", "Counter.read"])?;
        compile_and_execute(&compiler, &clang, &root.0, "overloaded", OVERLOADED,
            &["Box.read$arity0", "Box.read$arity1"])?;
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
        must_reject(&compiler, &root.0, "private_method",
            "define class Box as let int value=42; hidden():int{return self.value;} end pub fnc main():int{let Box b=new Box();return b.hidden();}",
            "private instance method")?;
        eprintln!("PASS: 2 compiled class guests, exact direct/overload symbols, O0/O2 exit 42, and 4 source rejections");
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
    fn identifies_exact_direct_calls() {
        let ir = "%v = call i64 @\"Counter.read\"(ptr %c)\n  call void @\"Counter.add\"(ptr %c, i64 2)";
        assert!(require_direct_method_calls(ir, &["Counter.read", "Counter.add"]).is_ok());
    }

    #[test]
    fn identifies_overload_slot_symbols() {
        let ir = "%v = call i64 @\"Box.read$arity0\"(ptr %c)\n%w = call i64 @\"Box.read$arity1\"(ptr %c, i64 2)";
        assert!(require_direct_method_calls(ir, &["Box.read$arity0", "Box.read$arity1"]).is_ok());
    }

    #[test]
    fn rejects_indirect_function_pointer_call() {
        let ir = "%v = call i64 %fn(ptr %c)\n%w = call i64 @\"Counter.read\"(ptr %c)";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }

    #[test]
    fn rejects_typed_function_pointer_call() {
        let ir = "%v = call i64 (ptr) %fn(ptr %c)\n%w = call i64 @\"Counter.read\"(ptr %c)";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }

    #[test]
    fn rejects_other_computed_call_forms_even_with_a_valid_direct_target() {
        let real = "%ok = call i64 @\"Counter.read\"(ptr %c)";
        for computed in [
            "%v = call i64 bitcast (ptr @other to ptr)(ptr %c)",
            "%v = call i64 asm sideeffect \"ret\", \"=r\"()",
            "%v = call i64 (ptr) %fn(ptr %c)",
        ] {
            let ir = format!("{real}\n{computed}");
            assert!(require_direct_method_calls(&ir, &["Counter.read"]).is_err(),
                "unexpected admission: {computed}");
        }
    }

    #[test]
    fn rejects_missing_method_and_declaration_spoofing() {
        let ir = "define i64 @\"Counter.read\"(ptr %c) { ret i64 42 }";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_err());
    }

    #[test]
    fn rejects_namespace_prefix_and_suffix_spoofing() {
        for imposter in ["Other.Counter.read", "Counter.read.evil",
                         "Counter.read$arity01", "Counter.read$arity0extra"] {
            let ir = format!("%v = call i64 @\"{imposter}\"(ptr %c)");
            assert!(require_direct_method_calls(&ir, &["Counter.read"]).is_err());
        }
    }

    #[test]
    fn rejects_wrong_overload_arity() {
        let ir = "%v = call i64 @\"Box.read$arity1\"(ptr %c, i64 2)";
        assert!(require_direct_method_calls(ir, &["Box.read$arity0"]).is_err());
    }

    #[test]
    fn recognizes_direct_invoke() {
        let ir = "%v = invoke i64 @\"Counter.read\"(ptr %c) to label %ok unwind label %fail";
        assert!(require_direct_method_calls(ir, &["Counter.read"]).is_ok());
    }
}
