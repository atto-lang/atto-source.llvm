// Std-only Rust runner for the real native C++ trace component. No Python,
// external dependencies, source mirroring, or mock guest execution.
use std::env;
use std::fs;
use std::path::PathBuf;
use std::process::Command;

fn command_ok(cmd: &mut Command, label: &str) {
    let output = cmd.output().unwrap_or_else(|error| panic!("{label}: {error}"));
    assert!(output.status.success(), "{label} failed (status {}):\n{}\n{}",
            output.status,
            String::from_utf8_lossy(&output.stdout),
            String::from_utf8_lossy(&output.stderr));
}

fn main() {
    let root = env::args_os().nth(1).map(PathBuf::from)
        .unwrap_or_else(|| env::current_dir().expect("repository root"));
    let root = root.canonicalize().expect("existing repository root");
    let source = root.join("tests/dop_aot_layout_trace_test.cpp");
    let include = root.join("src/main/cpp");
    assert!(source.is_file(), "real C++ native probe test must exist");
    let output_dir = env::temp_dir().join(format!("dop-aot-layout-{}", std::process::id()));
    fs::create_dir_all(&output_dir).expect("create temporary native test directory");
    for compiler in ["g++", "clang++"] {
        for mode in ["O0", "O2", "sanitizers"] {
            let binary = output_dir.join(format!("{}-{}", compiler, mode));
            let mut command = Command::new(compiler);
            command.arg("-std=c++17").arg("-Wall").arg("-Wextra")
                .arg("-Wpedantic").arg("-Werror").arg("-UNDEBUG")
                .arg(format!("-I{}", include.display()));
            if mode == "sanitizers" {
                command.args(["-O1", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]);
            } else {
                command.arg(format!("-{mode}"));
            }
            command.arg(&source).arg("-o").arg(&binary);
            command_ok(&mut command, &format!("{compiler} {mode} native build"));
            let mut execute = Command::new(&binary);
            if mode == "sanitizers" { execute.env("ASAN_OPTIONS", "detect_leaks=1"); }
            command_ok(&mut execute, &format!("{compiler} {mode} native execution"));
            println!("PASS {compiler} {mode}: real C++ host trace test");
        }
    }
    fs::remove_dir_all(&output_dir).expect("remove temporary binaries");
}
