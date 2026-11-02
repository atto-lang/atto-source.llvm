//! Real compiler admission: thirty-one denials and four genuine IR controls.
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
    // Snapshot the parent after creating the harness-owned log files.
    // A failed compiler must not leave artifacts alongside its fixture.
    let stdout_file = fs::File::create(&stdout_path).unwrap();
    let stderr_file = fs::File::create(&stderr_path).unwrap();
    let root_before = entries(root);
    let mut child = Command::new(compiler)
        .arg(&input)
        .current_dir(&dir)
        .stdout(Stdio::from(stdout_file))
        .stderr(Stdio::from(stderr_file))
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
    // The running-process check can race a compiler that exits immediately.
    // Enforce the output budget again before reading any potentially huge file.
    for stream_path in [&stdout_path, &stderr_path] {
        assert!(
            fs::metadata(stream_path).unwrap().len() <= 1024 * 1024,
            "{name}: compiler exceeded final output budget"
        );
    }
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
            status.code().is_some(),
            "{name}: signal-terminated compiler is not a controlled rejection"
        );
        assert!(
            stdout.is_empty(),
            "{name}: rejected source emitted IR: {stdout}"
        );
        assert!(
            stderr.contains(expected),
            "{name}: expected {expected:?}, got {stderr}"
        );
        // This test specifically proves the native mutable-share capability
        // gate, not merely a compiler failure containing matching words.
        // Mixed diagnostics can conceal an unrelated parser/host failure.
        if name.starts_with("mut-share-") || name.starts_with("six-mut-share-") {
            assert_eq!(status.code(), Some(1),
                "{name}: guarded-sharing denial requires controlled native compiler exit(1)");
            let lines: Vec<_> = stderr.lines().collect();
            assert_eq!(lines.len(), 1,
                "{name}: mixed native diagnostics are not guarded-sharing proof: {stderr}");
            let line = lines[0];
            assert!(line.starts_with(
                "oreslang-llvmc: native rt mut share requires verified SharedMutex<T> capability, guarded lowering and allocation-domain proof; refusing unsynchronized alias at byte "
            ), "{name}: compiler-owned guarded-sharing diagnostic required: {stderr}");
            let offset = line.rsplit_once(" at byte ").map(|(_, value)| value)
                .expect("guarded-sharing byte offset");
            assert!(!offset.is_empty() && offset.bytes().all(|byte| byte.is_ascii_digit()),
                "{name}: malformed or appended guarded-sharing diagnostic: {stderr}");
        }
        assert!(
            !has_definition(&stderr)
                && !stderr.lines().any(|line| {
                    let trimmed = line.trim_start();
                    // A partial function prototype or multiline definition
                    // is still LLVM IR, even if no opening brace was emitted.
                    (trimmed.starts_with("define ") && trimmed.contains('@'))
                        || trimmed.starts_with("; ModuleID")
                        || trimmed.starts_with("source_filename =")
                        || trimmed.starts_with("target triple =")
                        || trimmed.starts_with("target datalayout =")
                        || (trimmed.starts_with("declare ") && trimmed.contains('@'))
                        || (trimmed.starts_with("attributes #") && trimmed.contains('='))
                }),
            "{name}: rejected source leaked LLVM IR to stderr: {stderr}"
        );
        assert_eq!(entries(&dir), before, "{name}: rejection created artifacts");
        assert_eq!(
            entries(root), root_before,
            "{name}: rejection created artifacts beside the fixture"
        );
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
        ("mut-share-scalar-reject", "pub fnc main(): int { let int v = 42; return rt mut share v; }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-borrow-reject", "pub fnc main(): int { let int v = 42; return rt mut share (rt borrow v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-parenthesized-owned-reject", "pub fnc main(): int { let int v = 42; return rt mut share (v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-over-shared-handle-reject", "pub fnc main(): int { let int v = 42; return rt mut share (rt share v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-over-mutable-borrow-reject", "pub fnc main(): int { let int v = 42; return rt mut share (rt mut borrow v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),

        ("mut-share-local-binding-reject", "pub fnc main(): int { let int v = 42; let int alias = rt mut share v; return alias; }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-function-argument-reject", "fnc consume(int x): int { return x; }\npub fnc main(): int { let int v = 42; return consume(rt mut share v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-nested-group-reject", "pub fnc main(): int { let int v = 42; return rt mut share ((v)); }\n", "native rt mut share requires verified SharedMutex<T> capability"),

        ("mut-share-constructor-argument-reject", "pub define class Box as let int value=start;\n  pub constructor(int start){return;}\n  pub read()->int{return self.value;}\nend\npub fnc main(): int { let int v = 42; let Box box = new Box(rt mut share v); return box.read(); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-binary-expression-reject", "pub fnc main(): int { let int v = 42; return 1 + (rt mut share v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-borrowed-call-argument-reject", "fnc read(borrow int x): int { return x; }\npub fnc main(): int { let int v = 42; return read(rt mut share v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-nested-rt-borrow-reject", "pub fnc main(): int { let int v = 42; return rt borrow (rt mut share v); }\n", "native rt mut share requires verified SharedMutex<T> capability"),

        // Heap owners and shareable handles are not native shared-memory
        // lowering proofs. The dedicated unsupported operation must reject
        // before emitting *any* IR or files, even for nested ownership forms.
        ("mut-share-array-factory-reject", "pub fnc main(): int { return rt mut share (Array.new(42)); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-box-handle-reject", "pub fnc main(): int { return rt mut share (Box.new(42)); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-arc-handle-reject", "pub fnc main(): int { return rt mut share (Arc.new(42)); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-nested-take-reject", "pub fnc main(): int { let int owned = 42; return rt mut share (rt take owned); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("mut-share-nested-copy-reject", "pub fnc main(): int { let int owned = 42; return rt mut share (rt copy owned); }\n", "native rt mut share requires verified SharedMutex<T> capability"),

        // Shared six-expression source-denial intersection across the five toolchains.
        ("six-mut-share-scalar", "pub fnc main(): int { return rt mut share 42; }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("six-mut-share-parenthesized", "pub fnc main(): int { return (rt mut share 42); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("six-mut-share-binary", "pub fnc main(): int { return 1 + (rt mut share 2); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("six-mut-share-shared-borrow", "pub fnc main(): int { return rt mut share (rt borrow 3); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("six-mut-share-mutable-borrow", "pub fnc main(): int { return rt mut share (rt mut borrow 4); }\n", "native rt mut share requires verified SharedMutex<T> capability"),
        ("six-mut-share-recursive", "pub fnc main(): int { return rt mut share (rt mut share 5); }\n", "native rt mut share requires verified SharedMutex<T> capability"),

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
        ("valid-shared-borrow", "fnc read(borrow int x): int { return x; }\npub fnc main(): int { let int v = 42; return read(rt borrow v); }\n"),
        ("valid-exclusive-mutable-borrow", "fnc read(borrow mut int x): int { return x; }\npub fnc main(): int { let int v = 42; return read(rt mut borrow v); }\n"),
        ("valid-owned-scalar-call", "fnc consume(int x): int { return x; }\npub fnc main(): int { return consume(42); }\n"),
        ("valid-owned-constructor", "pub define class Box as let int value=start;\n  pub constructor(int start){return;}\n  pub read()->int{return self.value;}\nend\npub fnc main(): int { let Box box = new Box(42); return box.read(); }\n"),
    ] {
        check_case(&compiler, &root.0, name, source, None);
    }
    println!(
        "PASS native borrow admission: thirty-one no-IR/no-artifact denials, four real IR controls"
    );
}

#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(unix)]
    fn assert_rejected_fake_compiler(script_body: &str) {
        use std::os::unix::fs::PermissionsExt;

        let root = Scratch::new();
        let compiler = root.0.join("synthetic-compiler.sh");
        fs::write(&compiler, format!("#!/bin/sh\n{script_body}\n")).unwrap();
        let mut permissions = fs::metadata(&compiler).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&compiler, permissions).unwrap();
        let caught = std::panic::catch_unwind(|| {
            check_case(
                &compiler,
                &root.0,
                "invalid-mutable-share",
                "pub fnc main(): int { return rt mut share 42; }\n",
                Some("bad ownership"),
            );
        });
        assert!(caught.is_err(), "invalid compiler output escaped admission");
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_ir_leak_even_on_failed_compile() {
        assert_rejected_fake_compiler(
            "printf 'define i64 @main() {\n'\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_artifact_creation_on_failure() {
        assert_rejected_fake_compiler(
            "touch unapproved.ll\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_success_exit_with_diagnostic() {
        assert_rejected_fake_compiler("printf 'bad ownership\n' >&2\nexit 0");
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_wrong_diagnostic() {
        assert_rejected_fake_compiler("printf 'unrelated diagnostic\n' >&2\nexit 2");
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_source_rewrite() {
        assert_rejected_fake_compiler(
            "printf 'mutated source\n' > input.ores\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_source_replacement_with_symlink() {
        assert_rejected_fake_compiler(
            "rm input.ores\nln -s /dev/null input.ores\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn guarded_share_denial_rejects_mixed_native_diagnostics() {
        use std::os::unix::fs::PermissionsExt;
        let root = Scratch::new();
        let compiler = root.0.join("mixed-diagnostic-compiler.sh");
        let denial = "oreslang-llvmc: native rt mut share requires verified SharedMutex<T> capability, guarded lowering and allocation-domain proof; refusing unsynchronized alias at byte 49";
        fs::write(&compiler, format!(
            "#!/bin/sh\nprintf '%s\\n' '{}' >&2\nprintf 'error: unrelated parser failure\\n' >&2\nexit 1\n",
            denial
        )).unwrap();
        let mut permissions = fs::metadata(&compiler).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&compiler, permissions).unwrap();
        // Verify that this fixture genuinely launches and returns an owned
        // nonzero compiler-like exit. Otherwise catch_unwind would accept a
        // broken shebang or a spawn panic as proof of rejection.
        let probe = Command::new(&compiler).output().expect("fake compiler must execute");
        assert_eq!(probe.status.code(), Some(1));
        assert!(probe.stdout.is_empty());
        let probe_stderr = String::from_utf8(probe.stderr).unwrap();
        assert_eq!(probe_stderr.lines().count(), 2,
            "fixture must actually print the forged denial plus parser error");

        let caught = std::panic::catch_unwind(|| check_case(
            &compiler, &root.0, "mut-share-mixed-diagnostic",
            "pub fnc main(): int { return rt mut share 42; }\n",
            Some("native rt mut share requires verified SharedMutex<T> capability")
        ));
        assert!(caught.is_err(), "mixed diagnostics falsely proved guarded-sharing denial");
    }

    #[cfg(unix)]
    #[test]
    fn guarded_share_denial_rejects_unexpected_exit_status() {
        use std::os::unix::fs::PermissionsExt;
        let root = Scratch::new();
        let compiler = root.0.join("wrong-exit-compiler.sh");
        let denial = "oreslang-llvmc: native rt mut share requires verified SharedMutex<T> capability, guarded lowering and allocation-domain proof; refusing unsynchronized alias at byte 49";
        fs::write(&compiler, format!(
            "#!/bin/sh\nprintf '%s\\n' '{}' >&2\nexit 2\n", denial
        )).unwrap();
        let mut permissions = fs::metadata(&compiler).unwrap().permissions();
        permissions.set_mode(0o700);
        fs::set_permissions(&compiler, permissions).unwrap();
        let probe = Command::new(&compiler).output().expect("fake compiler must execute");
        assert_eq!(probe.status.code(), Some(2));
        assert!(probe.stdout.is_empty());
        assert_eq!(String::from_utf8(probe.stderr).unwrap().lines().count(), 1);
        let caught = std::panic::catch_unwind(|| check_case(
            &compiler, &root.0, "mut-share-wrong-exit",
            "pub fnc main(): int { return rt mut share 42; }\n",
            Some("native rt mut share requires verified SharedMutex<T> capability")
        ));
        assert!(caught.is_err(), "non-compiler exit status falsely proved guarded-sharing denial");
    }

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
    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_whitespace_stdout() {
        assert_rejected_fake_compiler(
            "printf '  \n'\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_non_ir_stdout_even_with_correct_diagnostic() {
        assert_rejected_fake_compiler(
            "printf 'compilation failed\n'\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_nested_output_directory() {
        assert_rejected_fake_compiler(
            "mkdir -p generated/objects\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_extra_symlink_artifact() {
        assert_rejected_fake_compiler(
            "ln -s input.ores output.ll\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_mixed_ir_and_failure_banner() {
        assert_rejected_fake_compiler(
            "printf '; ownership failed\ndefine i64 @main() {\n'\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_parent_scratch_artifact() {
        assert_rejected_fake_compiler(
            "touch ../unapproved.ll\nprintf 'bad ownership\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_signal_termination_with_matching_diagnostic() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\n' >&2\nkill -KILL $$",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_llvm_ir_written_to_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\ndefine i64 @main() {\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_partial_module_header_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\n; ModuleID = fake\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_llvm_declaration_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\ndeclare i64 @escaped(i64)\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_llvm_attribute_block_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\nattributes #0 = { nounwind }\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_target_datalayout_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\ntarget datalayout = x86_64\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_truncated_function_prototype_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\ndefine i64 @truncated(i64)\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_multiline_function_definition_on_stderr() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\ndefine i64 @truncated(i64)\n{\n' >&2\nexit 2",
        );
    }

    #[cfg(unix)]
    #[test]
    fn negative_admission_rejects_output_over_budget_after_fast_exit() {
        assert_rejected_fake_compiler(
            "printf 'bad ownership\n' >&2\ndd if=/dev/zero bs=1024 count=1025 2>/dev/null\nexit 2",
        );
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
