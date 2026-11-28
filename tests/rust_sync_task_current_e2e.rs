//! Exact native guest execution of the Rust LLVM AOT compiler's pure sync-task slice.
use std::{env, fs, path::PathBuf, process::Command, time::{SystemTime, UNIX_EPOCH}};

fn run(comp: &str, clang: &str, dir: &PathBuf, name: &str, src: &str, expected: Option<i32>, diagnostic: &str) {
    let path=dir.join(format!("{name}.atto"));
    fs::write(&path,src).unwrap();
    // This is the Rust atto-llvm CLI, NOT the C++ oreslang-llvmc CLI:
    // it requires input.atto -o output.ll --emit-llvm, writes IR to disk
    // atomically, and emits diagnostics on stderr.
    let ir=dir.join(format!("{name}.ll"));
    let result=Command::new(comp).arg(&path).arg("-o").arg(&ir).arg("--emit-llvm").output()
        .expect("native atto-llvm compiler executable required");
    assert!(result.stdout.is_empty(), "{name}: unexpected compiler stdout");
    if let Some(code)=expected {
        assert!(result.status.success(), "{name}: {:?}", String::from_utf8_lossy(&result.stderr));
        let llvm=fs::read_to_string(&ir).expect("Rust compiler wrote no LLVM IR file");
        assert!(llvm.contains("; InlineTaskEnter") && llvm.contains("; InlineTaskExit"), "{name}: no inline task IR");
        assert!(!llvm.contains("call void @atto_task_spawn"), "{name}: inline became spawn");
        let exe=dir.join(format!("{name}.native"));
        let cc=Command::new(clang).args(["-O1", "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer", "-Wno-override-module", "-x", "ir"])
            .arg(&ir).arg("-o").arg(&exe).output().unwrap();
        assert!(cc.status.success(), "{name}: clang: {}", String::from_utf8_lossy(&cc.stderr));
        let guest=Command::new(&exe).env("ASAN_OPTIONS","detect_leaks=1:halt_on_error=1")
            .env("UBSAN_OPTIONS","halt_on_error=1").output().unwrap();
        assert_eq!(guest.status.code(),Some(code),
            "{name}: native guest: {}", String::from_utf8_lossy(&guest.stderr));
    } else {
        assert!(!result.status.success(), "{name} unexpectedly compiled");
        assert!(!ir.exists(), "{name}: compiler left partial LLVM IR on rejected source");
        assert!(String::from_utf8_lossy(&result.stderr).contains(diagnostic),
            "{name}: missing {diagnostic}: {}",String::from_utf8_lossy(&result.stderr));
    }
}
fn main() {
    let a: Vec<_> = env::args().collect();
    assert_eq!(a.len(), 3, "usage: rust_sync_task_current_e2e atto-llvm clang");
    let root = env::temp_dir().join(format!("atto-sync-{}-{}", std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));
    fs::create_dir(&root).unwrap();
    run(&a[1], &a[2], &root, "typed_writeback", "pub fnc main(): int { let int n = 0; val int r = sync task<int> { n = 2; return 40; }; return n+r; }", Some(42), "");
    run(&a[1], &a[2], &root, "inferred_nested", "pub fnc main(): int { val int r = sync task { return sync task<int> { return 40; } + 2; }; return r; }", Some(42), "");
    run(&a[1], &a[2], &root, "void_writeback", "pub fnc main(): int { let int n = 40; do sync task<void> { n = n + 2; return; }; return n; }", Some(42), "");
    run(&a[1], &a[2], &root, "lexical_shadow", "pub fnc main(): int { let int n = 42; do sync task { val int n = 1; }; return n; }", Some(42), "");
    run(&a[1], &a[2], &root, "nested_void", "pub fnc main(): int { let int n = 39; do sync task { n = n + 1; do sync task<void> { n = n + 2; }; }; return n; }", Some(42), "");
    run(&a[1], &a[2], &root, "selected_ternary", "pub fnc main(): int { let int n = 0; val int r = true ? sync task<int> { n = 2; return 40; } : sync task<int> { n = 9; return 0; }; return r+n; }", Some(42), "");
    run(&a[1], &a[2], &root, "bare_void", "pub fnc main(): int { sync task {}; return 0; }", None, "E_SYNTAX");
    run(&a[1], &a[2], &root, "parenthesized_discard", "pub fnc main(): int { (sync task<int> { return 42; }); return 0; }", None, "E_SYNTAX");
    run(&a[1], &a[2], &root, "do_typed_value", "pub fnc main(): int { do sync task<int> { return 1; }; return 0; }", None, "E_TASK_RETURN_TYPE");
    run(&a[1], &a[2], &root, "do_inferred_value", "pub fnc main(): int { do sync task { return 1; }; return 0; }", None, "E_TASK_RETURN_TYPE");
    run(&a[1], &a[2], &root, "wrong_type", "pub fnc main(): int { val int n = sync task<bool> { return 1; }; return n; }", None, "E_TASK_RETURN_TYPE");
    run(&a[1], &a[2], &root, "unproven_call", "pub fnc main(): int { return sync task<int> { return helper(); }; }", None, "E_UNSUPPORTED");
    run(&a[1], &a[2], &root, "await", "pub fnc main(): int { val int n = sync task<int> { return await future; }; return n; }", None, "E_UNSUPPORTED");
    run(&a[1], &a[2], &root, "spawn", "pub fnc main(): int { val int n = task<int> { return 42; }; return n; }", None, "E_UNSUPPORTED");
    run(&a[1], &a[2], &root, "missing_return", "pub fnc main(): int { val int n = sync task<int> {}; return n; }", None, "E_TASK_RETURN_TYPE");
    fs::remove_dir_all(&root).unwrap();
    println!("ATTO_LLVM_INLINE_TASK_CURRENT_GUESTS=6 NEGATIVE=9");
}
