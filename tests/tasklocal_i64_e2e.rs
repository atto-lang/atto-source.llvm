use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct Scratch(PathBuf);
impl Scratch {
    fn new() -> Self {
        let stamp = SystemTime::now().duration_since(UNIX_EPOCH)
            .expect("clock").as_nanos();
        let path = std::env::temp_dir().join(
            format!("atto-tasklocal-{}-{stamp}", std::process::id()));
        fs::create_dir(&path).expect("create scratch");
        Self(path)
    }
}
impl Drop for Scratch {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

fn compile(compiler: &Path, dir: &Path, name: &str, source: &str)
    -> std::process::Output
{
    let input = dir.join(name);
    fs::write(&input, source).expect("write source");
    let output = Command::new(compiler).arg(&input)
        .current_dir(dir).output().expect("run LLVM compiler");
    assert_eq!(fs::read_to_string(&input).expect("roundtrip source"), source);
    output
}

fn reject(compiler: &Path, dir: &Path, name: &str,
          source: &str, diagnostic: Option<&str>) {
    let output = compile(compiler, dir, name, source);
    assert!(!output.status.success(), "{name}: unexpected acceptance");
    assert!(output.stdout.iter().all(u8::is_ascii_whitespace),
        "{name}: rejected source emitted IR");
    if let Some(message) = diagnostic {
        let stderr = String::from_utf8_lossy(&output.stderr);
        assert!(stderr.contains(message),
            "{name}: missing {message:?}, stderr={stderr}");
    }
}

fn main() {
    let mut args = std::env::args_os().skip(1);
    let compiler = PathBuf::from(args.next().expect("COMPILER"))
        .canonicalize().expect("compiler path");
    let clang = PathBuf::from(args.next().expect("CLANG"))
        .canonicalize().expect("clang path");
    let root = PathBuf::from(args.next().expect("SOURCE_ROOT"))
        .canonicalize().expect("source root");
    let dir = Scratch::new();

    let positive = fs::read_to_string(
        root.join("tests/fixtures/tasklocal-i64-source.atto"))
        .expect("canonical .atto fixture");
    let output = compile(&compiler, &dir.0, "tasklocal.atto", &positive);
    assert!(output.status.success(),
        "TaskLocal<int> compiler rejected fixture: {}",
        String::from_utf8_lossy(&output.stderr));
    let ir = String::from_utf8_lossy(&output.stdout);
    for symbol in ["; ModuleID", "oreslang_source_task_start_i64",
                   "oreslang_source_task_local_i64_set",
                   "oreslang_source_task_local_i64_get",
                   "oreslang_source_task_await_i64",
                   "oreslang_source_task_runtime_shutdown"] {
        assert!(ir.contains(symbol), "missing IR symbol: {symbol}");
    }

    let ll = dir.0.join("tasklocal.ll");
    let exe = dir.0.join("tasklocal");
    fs::write(&ll, &output.stdout).expect("write compiler IR");
    let include = root.join("src/main/cpp");
    let runtime = root.join("src/main/cpp/oreslang/runtime/SourceTaskRuntime.c");
    let scheduler = root.join("src/main/cpp/oreslang/runtime/NativeActorScheduler.c");
    let linked = Command::new(&clang)
        .arg(&ll).arg(&runtime).arg(&scheduler)
        .arg("-I").arg(&include).arg("-std=c11").arg("-pthread")
        .arg("-DORESLANG_SOURCE_TASK_TESTING=1")
        .arg("-o").arg(&exe)
        .current_dir(&dir.0).output().expect("native link invocation");
    assert!(linked.status.success(),
        "TaskLocal<int> native link failed: {}",
        String::from_utf8_lossy(&linked.stderr));
    let run = Command::new(&exe).current_dir(&dir.0).output()
        .expect("run compiler-produced native binary");
    assert_eq!(run.status.code(), Some(42),
        "compiled TaskLocal<int> should yield 42: {}",
        String::from_utf8_lossy(&run.stderr));
    let trace = String::from_utf8_lossy(&run.stderr);
    assert!(trace.contains("SOURCE_TASK_RUNTIME_PASS"));
    assert!(trace.contains("producer_on_carrier=1"));

    reject(&compiler, &dir.0, "tasklocal-missing-set.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { return TaskLocal<int>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("requires a preceding TaskLocal<int>.set"));
    reject(&compiler, &dir.0, "tasklocal-wrong-type.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<bool>.set(5); return TaskLocal<int>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        None);
    reject(&compiler, &dir.0, "tasklocal-bool-value.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int>.set(true); return TaskLocal<int>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("TaskLocal<int>.set requires a scalar int value"));
    reject(&compiler, &dir.0, "tasklocal-not-in-task.atto",
        "pub fnc main(): int {\nreturn TaskLocal<int>.get();\n}\n",
        None);
    reject(&compiler, &dir.0, "tasklocal-raw-ffi.atto",
        "pub fnc main(): int {\nreturn oreslang_source_task_local_i64_get();\n}\n",
        None);
    reject(&compiler, &dir.0, "tasklocal-no-await.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int>.set(4); return TaskLocal<int>.get(); };\nreturn 0;\n}\n",
        Some("must be awaited exactly once"));
    reject(&compiler, &dir.0, "tasklocal-double-set.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int>.set(4); TaskLocal<int>.set(5); return TaskLocal<int>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        None);
    reject(&compiler, &dir.0, "tasklocal-abi-shadow.atto",
        "fnc oreslang_source_task_local_i64_get(): int { return 7; }\npub fnc main(): int { return oreslang_source_task_local_i64_get(); }\n",
        Some("reserved TaskLocal runtime ABI"));
    println!("TASKLOCAL_I64_COMPILER_GUEST_PASS: 1 compiled binary and 8 rejected fixtures");
}
