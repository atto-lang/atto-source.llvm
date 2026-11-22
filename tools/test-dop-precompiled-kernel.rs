//! Std-only Rust runner. Executes real native host tests, not Atto guest IR.
//! Usage: rustc --edition 2021 tools/test-dop-precompiled-kernel.rs -o /tmp/verify-dop-aot
//!        /tmp/verify-dop-aot .
use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn run(command: &mut Command, label: &str) {
    let result = command.output().unwrap_or_else(|err| panic!("{label}: {err}"));
    assert!(
        result.status.success(),
        "{label}: exit {}\nstdout:\n{}\nstderr:\n{}",
        result.status,
        String::from_utf8_lossy(&result.stdout),
        String::from_utf8_lossy(&result.stderr),
    );
}

fn main() {
    let root = env::args_os().nth(1).map(PathBuf::from)
        .unwrap_or_else(|| env::current_dir().expect("cwd"));
    let root = root.canonicalize().expect("valid repo root");
    let source = root.join("tests/dop_precompiled_kernel_region_test.cpp");
    assert!(source.is_file(), "real production-backed C++ test source is missing");
    let include = root.join("src/main/cpp");
    let dir = env::temp_dir().join(format!("dop-precompiled-{}", std::process::id()));
    fs::create_dir_all(&dir).expect("create temporary directory");
    for cc in ["g++", "clang++"] {
        for mode in ["O0", "O2", "san"] {
            let binary = dir.join(format!("{cc}-{mode}"));
            let mut build = Command::new(cc);
            build.args(["-std=c++17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG"]);
            build.arg(format!("-I{}", include.display()));
            if mode == "san" {
                build.args(["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]);
            } else {
                build.arg(format!("-{mode}"));
            }
            build.arg(&source).arg("-o").arg(&binary);
            run(&mut build, &format!("compile {cc} {mode}"));
            let mut execute = Command::new(&binary);
            if mode == "san" {
                execute.env("ASAN_OPTIONS", "detect_leaks=1");
            }
            run(&mut execute, &format!("execute {cc} {mode}"));
            println!("PASS {cc} {mode}: native host checked-region AOT dispatch");
        }
    }
    fs::remove_dir_all(dir).expect("remove test binaries");
}
