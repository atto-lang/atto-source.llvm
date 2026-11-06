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

    // An independent source program asks the compiler to assign TWO
    // distinct typed key descriptors. The LLVM producer must invoke the
    // checked keyed native ABI twice and read the second static slot.
    let keyed = fs::read_to_string(
        root.join("tests/fixtures/tasklocal-keyed-i64-source.atto"))
        .expect("named TaskLocal<int> guest fixture");
    let compiled = compile(&compiler, &dir.0, "tasklocal-keyed.atto", &keyed);
    assert!(compiled.status.success(),
        "typed TaskLocal<int> compiler rejected fixture: {}",
        String::from_utf8_lossy(&compiled.stderr));
    let keyed_ir = String::from_utf8_lossy(&compiled.stdout);
    assert_eq!(keyed_ir.matches(
        "call void @oreslang_source_task_local_i64_set_keyed").count(), 2,
        "two distinct static typed keys must be emitted");
    assert!(keyed_ir.contains(
        "call i64 @oreslang_source_task_local_i64_get_keyed"),
        "typed key getter must use checked runtime ABI");
    for symbol in [
        "oreslang_source_task_start_i64",
        "oreslang_source_task_runtime_shutdown"] {
        assert!(keyed_ir.contains(symbol), "missing keyed IR symbol: {symbol}");
    }
    let keyed_ll = dir.0.join("tasklocal-keyed.ll");
    let keyed_exe = dir.0.join("tasklocal-keyed");
    fs::write(&keyed_ll, &compiled.stdout).expect("write typed LLVM IR");
    let linked = Command::new(&clang)
        .arg(&keyed_ll).arg(&runtime).arg(&scheduler)
        .arg("-I").arg(&include).arg("-std=c11").arg("-pthread")
        .arg("-DORESLANG_SOURCE_TASK_TESTING=1")
        .arg("-o").arg(&keyed_exe)
        .current_dir(&dir.0).output().expect("typed native link invocation");
    assert!(linked.status.success(),
        "typed TaskLocal<int> native link failed: {}",
        String::from_utf8_lossy(&linked.stderr));
    let executed = Command::new(&keyed_exe).current_dir(&dir.0).output()
        .expect("run compiler-generated typed native TaskLocal binary");
    assert_eq!(executed.status.code(), Some(42),
        "typed key native guest should yield 42: {}",
        String::from_utf8_lossy(&executed.stderr));
    assert!(String::from_utf8_lossy(&executed.stderr)
        .contains("producer_on_carrier=1"));

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
    reject(&compiler, &dir.0, "tasklocal-named-missing-set.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int, key=a>.set(4); return TaskLocal<int, key=b>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("requires a preceding TaskLocal<int>.set"));
    reject(&compiler, &dir.0, "tasklocal-named-double-set.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int, key=a>.set(4); TaskLocal<int, key=a>.set(5); return TaskLocal<int, key=a>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("duplicate static key"));
    reject(&compiler, &dir.0, "tasklocal-named-over-quota.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int, key=a>.set(1); TaskLocal<int, key=b>.set(2); TaskLocal<int, key=c>.set(3); TaskLocal<int, key=d>.set(4); TaskLocal<int, key=e>.set(5); TaskLocal<int, key=f>.set(6); TaskLocal<int, key=g>.set(7); TaskLocal<int, key=h>.set(8); TaskLocal<int, key=i>.set(9); return TaskLocal<int, key=i>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("exceeds eight static keys"));
    reject(&compiler, &dir.0, "tasklocal-named-wrong-type.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int, key=a>.set(true); return TaskLocal<int, key=a>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("TaskLocal<int>.set requires a scalar int value"));
    reject(&compiler, &dir.0, "tasklocal-named-reserved.atto",
        "pub fnc main(): int {\nlet Task<int> w = task { TaskLocal<int, key=default>.set(5); return TaskLocal<int, key=default>.get(); };\nlet int v = await w;\nreturn v;\n}\n",
        Some("reserves key name default"));
    reject(&compiler, &dir.0, "tasklocal-named-ffi-shadow.atto",
        "fnc oreslang_source_task_local_i64_get_keyed(): int { return 7; }\npub fnc main(): int { return oreslang_source_task_local_i64_get_keyed(); }\n",
        Some("reserved native runtime ABI symbol"));
    reject(&compiler, &dir.0, "tasklocal-abi-shadow.atto",
        "fnc oreslang_source_task_local_i64_get(): int { return 7; }\npub fnc main(): int { return oreslang_source_task_local_i64_get(); }\n",
        Some("reserved native runtime ABI symbol"));
    println!("TASKLOCAL_I64_COMPILER_GUEST_PASS: 2 compiled binaries and 14 rejected fixtures");
}
