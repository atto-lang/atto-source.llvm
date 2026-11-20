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
    let a:Vec<_>=env::args().collect();
    assert_eq!(a.len(),3,"usage: rust_sync_task_e2e atto-llvm clang");
    let root=env::temp_dir().join(format!("atto-sync-{}-{}",std::process::id(),
        SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos()));
    fs::create_dir(&root).unwrap();
    run(&a[1],&a[2],&root,"typed_captured",
        "pub fnc main(): int { val int base = 8; val int got = sync task<int> { val int v = base + 2; return v + 3; }; return got; }",
        Some(13),"");
    run(&a[1],&a[2],&root,"inferred_nested",
        "pub fnc main(): int { val int got = sync task { return sync task<int> { return 5; } + 2; }; return got; }",
        Some(7),"");
    run(&a[1],&a[2],&root,"do_explicit_void",
        "pub fnc main(): int { do sync task<void> { val int n = 1; return; }; return 43; }",
        Some(43),"");
    run(&a[1],&a[2],&root,"do_inferred_void",
        "pub fnc main(): int { do sync task { val int n = 1; }; return 44; }",
        Some(44),"");
    run(&a[1],&a[2],&root,"do_empty_void",
        "pub fnc main(): int { do sync task<void> {}; return 45; }",
        Some(45),"");
    run(&a[1],&a[2],&root,"empty_void",
        "pub fnc main(): int { sync task {}; return 46; }",
        Some(46),"");
    run(&a[1],&a[2],&root,"standalone_void_explicit",
        "pub fnc main(): int { sync task<void> { return; }; return 47; }",
        Some(47),"");
    run(&a[1],&a[2],&root,"caller_local_order",
        "pub fnc main(): int { let int n = 1; do sync task<void> { n = n + 5; do sync task { n = n + 7; }; }; return n; }",
        Some(13),"");
    run(&a[1],&a[2],&root,"lexical_shadow",
        "pub fnc main(): int { let int n = 7; do sync task { val int n = 12; }; return n; }",
        Some(7),"");
    run(&a[1],&a[2],&root,"invalid_type",
        "pub fnc main(): int { val int n = sync task<bool> { return 1; }; return n; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_typed_nonvoid",
        "pub fnc main(): int { do sync task<int> { return 1; }; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_typed_nonvoid_empty",
        "pub fnc main(): int { do sync task<int> {}; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_inferred_return",
        "pub fnc main(): int { do sync task { return 1; }; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_void_value",
        "pub fnc main(): int { do sync task<void> { return 1; }; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_type_precedes_effect",
        "pub fnc main(): int { do sync task<int> { return await future; }; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"invalid_do_generic_precedes_effect",
        "pub fnc main(): int { do sync task<Result<int,string>> { return await future; }; return 0; }",
        None,"E_TASK_RETURN_TYPE");
    run(&a[1],&a[2],&root,"unverified_effect",
        "pub fnc helper(): int { return 8; } pub fnc main(): int { return sync task<int> { return helper(); }; }",
        None,"E_UNSUPPORTED");
    run(&a[1],&a[2],&root,"no_spawn",
        "pub fnc main(): int { val int n = task<int> { return 2; }; return n; }",
        None,"E_UNSUPPORTED");
    fs::remove_dir_all(&root).unwrap();
    println!("ATTO_LLVM_SYNC_TASK_REAL_GUESTS=9 NEGATIVE=9");
}
