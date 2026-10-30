//! Real compiler admission: nine denials and four genuine IR controls.
use std::{
    collections::BTreeSet,
    env, fs,
    path::{Path, PathBuf},
    process::{Command, Stdio},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_nanos();
        let path = env::temp_dir().join(format!("ores-borrow-{}-{stamp}", std::process::id()));
        fs::create_dir(&path).unwrap();
        Self(path)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn entries(path: &Path) -> BTreeSet<std::ffi::OsString> {
    fs::read_dir(path)
        .unwrap()
        .map(|e| e.unwrap().file_name())
        .collect()
}
fn has_definition(output: &str) -> bool {
    output.lines().any(|line| {
        line.trim_start()
            .strip_prefix("define")
            .is_some_and(|rest| {
                rest.chars().next().is_some_and(char::is_whitespace)
                    && rest.contains('@')
                    && rest.contains('(')
                    && rest.trim_end().ends_with('{')
            })
    })
}

fn check_case(compiler: &Path, root: &Path, name: &str, source: &str, diagnostic: Option<&str>) {
    let dir = root.join(name);
    fs::create_dir(&dir).unwrap();
    let input = dir.join("input.ores");
    fs::write(&input, source).unwrap();
    let before = entries(&dir);
    // Capture outside the fixture directory so any compiler-created entry,
    // including a directory, remains observable as an admission failure.
    let stdout_path = root.join(format!("{name}.stdout"));
    let stderr_path = root.join(format!("{name}.stderr"));
    let mut child = Command::new(compiler)
        .arg(&input)
        .current_dir(&dir)
        .stdout(Stdio::from(fs::File::create(&stdout_path).unwrap()))
        .stderr(Stdio::from(fs::File::create(&stderr_path).unwrap()))
        .spawn()
        .expect("start compiler");
    let deadline = Instant::now() + Duration::from_secs(20);
    let status = loop {
        if let Some(status) = child.try_wait().unwrap() {
            break status;
        }
        let excessive = [&stdout_path, &stderr_path]
            .iter()
            .any(|p| fs::metadata(p).unwrap().len() > 1024 * 1024);
        if Instant::now() >= deadline || excessive {
            let _ = child.kill();
            let _ = child.wait();
            panic!("{name}: compiler exceeded time/output budget");
        }
        thread::sleep(Duration::from_millis(10));
    };
    let stdout = fs::read_to_string(stdout_path).unwrap();
    let stderr = fs::read_to_string(stderr_path).unwrap();
    assert!(
        fs::symlink_metadata(&input).unwrap().file_type().is_file(),
        "{name}: replaced input"
    );
    assert_eq!(
        fs::read(&input).unwrap(),
        source.as_bytes(),
        "{name}: source mutated"
    );
    if let Some(expected) = diagnostic {
        assert!(!status.success(), "{name}: invalid source admitted");
        assert!(
            stdout.trim().is_empty(),
            "{name}: rejected source emitted IR: {stdout}"
        );
        assert!(
            stderr.contains(expected),
            "{name}: expected {expected:?}, got {stderr}"
        );
        assert_eq!(entries(&dir), before, "{name}: rejection created artifacts");
    } else {
        assert!(
            status.success() && has_definition(&stdout),
            "{name}: missing real IR: {stderr}"
        );
    }
}

fn main() {
    let args: Vec<_> = env::args_os().collect();
    assert_eq!(args.len(), 2, "usage: native_borrow_admission COMPILER");
    let compiler = fs::canonicalize(&args[1]).expect("compiler path");
    let root = Scratch::new();
    let negatives = [
        ("immutable-local-write", "pub fnc main(): int { val int count = 1; count = 2; return count; }\n", "assignment requires an unloaned mutable scalar slot"),
        ("implicit-borrow", "fnc get(borrow int x): int { return x; }\npub fnc main(): int { let int v = 42; return get(v); }\n", "borrowed argument requires explicit rt borrow"),
        ("wrong-borrow-mutability", "fnc get(borrow mut int x): int { return x; }\npub fnc main(): int { let int v = 42; return get(rt borrow v); }\n", "borrow mode does not match borrowed parameter"),
        ("temporary-borrow", "fnc get(borrow int x): int { return x; }\npub fnc main(): int { return get(rt borrow (40 + 2)); }\n", "native borrow requires an addressable named int"),
        ("shared-to-mutable", "fnc edit(borrow mut int x): int { return x; }\nfnc inner(borrow int x): int { return edit(rt mut borrow x); }\npub fnc main(): int { let int v = 42; return inner(rt borrow v); }\n", "cannot mutably reborrow shared parameter"),
        ("borrow-to-owned-parameter", "fnc consume(int x): int { return x; }\npub fnc main(): int { let int v = 42; return consume(rt borrow v); }\n", "borrowed reference may only be passed to a borrowed parameter"),
        ("borrow-to-owned-constructor", "pub define class Box as let int value=start;\n  pub constructor(int start){return;}\n  pub read()->int{return self.value;}\nend\npub fnc main(): int { let int v = 42; let Box box = new Box(rt borrow v); return box.read(); }\n", "native scalar constructor argument requires pure integer expression"),
        ("generator-yield-loan-unsupported", "generator fnc values(): int { let int x = 1; yield rt borrow x; return; }\n", "bare native generator declaration requires 'generator*'"),
        ("canonical-borrowed-yield-unsupported", "gen* values(): Iterator<int> { yield rt borrow x; return; }\n", "expected integer literal"),
    ];
    for (name, source, expected) in negatives {
        check_case(&compiler, &root.0, name, source, Some(expected));
    }
    for (name, source) in [
        ("valid-local-in-place-mutation", "pub fnc main(): int { let int count = 1; count = count + 1; return count; }\n"),
        ("valid-shared-borrow", "fnc read(borrow int x): int { return x; }\npub fnc main(): int { let int v = 42; return read(rt borrow v); }\n"),
        ("valid-owned-scalar-call", "fnc consume(int x): int { return x; }\npub fnc main(): int { return consume(42); }\n"),
        ("valid-owned-constructor", "pub define class Box as let int value=start;\n  pub constructor(int start){return;}\n  pub read()->int{return self.value;}\nend\npub fnc main(): int { let Box box = new Box(42); return box.read(); }\n"),
    ] {
        check_case(&compiler, &root.0, name, source, None);
    }
    println!(
        "PASS native borrow admission: nine no-IR/no-artifact denials, four real IR controls"
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn definition_predicate_requires_actual_function_line() {
        for output in [
            "define i64 @main() {",
            "  define internal i64 @f(i64 %x) {",
            "\tdefine void @f() {\nret void\n}",
        ] {
            assert!(has_definition(output));
        }
        for output in [
            "; define i64 @fake() {",
            "banner: define i64 @fake() {",
            "defined i64 @fake() {",
            "define a badge",
            "define @bad()",
        ] {
            assert!(!has_definition(output));
        }
    }
    #[test]
    fn artifact_inventory_includes_created_directories() {
        let root = Scratch::new();
        fs::write(root.0.join("input.ores"), "source").unwrap();
        let before = entries(&root.0);
        fs::create_dir(root.0.join("unexpected")).unwrap();
        assert_ne!(entries(&root.0), before);
    }
}
