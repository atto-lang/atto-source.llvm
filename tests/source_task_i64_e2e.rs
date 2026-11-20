use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct Temp(PathBuf);
impl Temp {
    fn new() -> Self {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock").as_nanos();
        let path = std::env::temp_dir().join(format!(
            "ores-task-source-{}-{stamp}", std::process::id()));
        fs::create_dir(&path).expect("create temp");
        Self(path)
    }
}
impl Drop for Temp {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

fn compile(compiler: &Path, dir: &Path, name: &str, source: &str)
    -> std::process::Output
{
    let input = dir.join(name);
    fs::write(&input, source).expect("write source");
    let output = Command::new(compiler)
        .arg(&input)
        .current_dir(dir)
        .output()
        .expect("run oreslang-llvmc");
    assert_eq!(fs::read_to_string(&input).expect("read source"), source);
    output
}

fn rejected(compiler: &Path, dir: &Path, name: &str, source: &str, diagnostic: &str) {
    let output = compile(compiler, dir, name, source);
    assert!(!output.status.success(), "{name}: expected rejection");
    assert!(output.stdout.iter().all(u8::is_ascii_whitespace),
        "{name}: rejection emitted LLVM IR");
    let stderr = String::from_utf8_lossy(&output.stderr);
    assert!(stderr.contains(diagnostic),
        "{name}: missing diagnostic {diagnostic:?}; stderr={stderr}");
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let compiler = PathBuf::from(args.next()
        .expect("usage: task-e2e COMPILER CLANG SOURCE_ROOT"))
        .canonicalize().expect("compiler");
    let clang = PathBuf::from(args.next().expect("missing clang"))
        .canonicalize().expect("clang");
    let root = PathBuf::from(args.next().expect("missing source root"))
        .canonicalize().expect("source root");
    let dir = Temp::new();

    let positive = fs::read_to_string(
        root.join("tests/fixtures/source-task-i64-await.ores"))
        .expect("read canonical Task fixture");
    let output = compile(&compiler, &dir.0, "task.ores", &positive);
    assert!(output.status.success(),
        "task source compile failed: {}", String::from_utf8_lossy(&output.stderr));
    assert!(output.stdout.starts_with(b"; ModuleID"),
        "task source did not emit LLVM IR");
    let ir = String::from_utf8_lossy(&output.stdout);
    for needle in [
        "oreslang_source_task_start_i64",
        "oreslang_source_task_await_i64",
        "oreslang_source_task_runtime_shutdown",
        ".task.w.producer",
    ] {
        assert!(ir.contains(needle), "missing task IR marker {needle}");
    }

    let ll = dir.0.join("task.ll");
    let exe = dir.0.join(if cfg!(windows) { "task.exe" } else { "task" });
    fs::write(&ll, &output.stdout).expect("write IR");
    let include = root.join("src/main/cpp");
    let runtime = root.join("src/main/cpp/oreslang/runtime/SourceTaskRuntime.c");
    let scheduler = root.join("src/main/cpp/oreslang/runtime/NativeActorScheduler.c");
    let build = Command::new(&clang)
        .arg(&ll)
        .arg(&runtime)
        .arg(&scheduler)
        .arg("-I").arg(&include)
        .arg("-std=c11")
        .arg("-pthread")
        .arg("-DORESLANG_SOURCE_TASK_TESTING=1")
        .arg("-o").arg(&exe)
        .current_dir(&dir.0)
        .output().expect("run clang");
    assert!(build.status.success(),
        "task native link failed: {}", String::from_utf8_lossy(&build.stderr));

    let run = Command::new(&exe).current_dir(&dir.0)
        .output().expect("run task native binary");
    assert_eq!(run.status.code(), Some(42),
        "task native result must be 42; stderr={}",
        String::from_utf8_lossy(&run.stderr));
    let trace = String::from_utf8_lossy(&run.stderr);
    assert!(trace.contains("SOURCE_TASK_RUNTIME_PASS"));
    assert!(trace.contains("producer_on_carrier=1"));
    assert!(trace.contains("carriers=1"));

    rejected(&compiler, &dir.0, "missing-await.ores",
        r#"pub fnc main(): int {
    let Task<int> w = task { return 41; };
    return 0;
}
"#, "must be awaited exactly once");

    rejected(&compiler, &dir.0, "double-await.ores",
        r#"pub fnc main(): int {
    let Task<int> w = task { return 41; };
    let int a = await w;
    let int b = await w;
    return a + b;
}
"#, "requires await to immediately consume");

    rejected(&compiler, &dir.0, "call-producer.ores",
        r#"fnc helper(): int { return 41; }
pub fnc main(): int {
    let Task<int> w = task { return helper(); };
    let int v = await w;
    return v;
}
"#, "producer must be capture-free and scalar");

    rejected(&compiler, &dir.0, "non-main.ores",
        r#"fnc work(): int {
    let Task<int> w = task { return 41; };
    let int v = await w;
    return v;
}
pub fnc main(): int { return work(); }
"#, "requires zero-argument top-level main");

    rejected(&compiler, &dir.0, "second-task.ores",
        r#"pub fnc main(): int {
    let Task<int> a = task { return 1; };
    let int av = await a;
    let Task<int> b = task { return 2; };
    let int bv = await b;
    return av + bv;
}
"#, "supports exactly one source Task");

    println!("PASS: source Task<int> submit -> carrier producer -> host await -> native 42; negative admission matrix");
}
