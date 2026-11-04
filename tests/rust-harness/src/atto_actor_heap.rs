//! A compiler-generated .atto actor handler linked with the real native C
//! actor-owned heap and harness under ASan+UBSan. Not guest scheduler TSan.
use super::{require, run, stderr, stdout, TempDir};
use std::ffi::OsStr;
use std::fs;
use std::path::Path;
use std::process::Command;

pub(super) fn execute(
    compiler: &Path,
    clang: &Path,
    root: &Path,
) -> Result<(), String> {
    require(
        compiler.is_file() && clang.is_file(),
        "real compiler and clang executables are required",
    )?;
    let atto = root.join("tests/fixtures/atto-actor-owned-fields.atto");
    let legacy = root.join("tests/fixtures/actor-owned-fields.ores");
    let source = fs::read(&atto).map_err(|e| format!("read .atto actor fixture: {e}"))?;
    let previous = fs::read(&legacy).map_err(|e| format!("read legacy actor fixture: {e}"))?;
    require(
        source == previous && !source.is_empty(),
        "AttoLang actor fixture must remain byte-identical to the working .ores fixture",
    )?;

    let temp = TempDir::new("atto-llvm-actor-heap-").map_err(|e| e.to_string())?;
    let ir = temp.path.join("actor.ll");
    let object = temp.path.join("actor.o");
    let executable = temp.path.join("actor-sanitized");

    let compiled = run(
        compiler,
        [OsStr::new("--target=host"), atto.as_os_str()],
    )?;
    require(
        compiled.status.success()
            && compiled.stderr.is_empty()
            && compiled.stdout.len() <= 2_000_000
            && stdout(&compiled).starts_with("; ModuleID")
            && stdout(&compiled).contains("oreslang_actor_Counter_on_message"),
        format!("real .atto actor lowering unavailable: {}", stderr(&compiled)),
    )?;
    fs::write(&ir, &compiled.stdout).map_err(|e| e.to_string())?;

    let shared = [
        "-O1",
        "-g",
        "-fno-omit-frame-pointer",
        "-fno-sanitize-recover=all",
        "-fsanitize=address,undefined",
    ];
    let object_result = Command::new(clang)
        .args(shared)
        .args(["-x", "ir", "-c"])
        .arg(&ir)
        .arg("-o")
        .arg(&object)
        .output()
        .map_err(|e| e.to_string())?;
    require(
        object_result.status.success(),
        format!("cannot instrument LLVM guest IR: {}", stderr(&object_result)),
    )?;

    let include = format!("-I{}", root.join("src/main/cpp").display());
    let linked = Command::new(clang)
        .args(shared)
        .args(["-std=c11", "-pthread"])
        .arg(include)
        .arg(root.join("tests/native_actor_source_harness.c"))
        .arg(root.join("src/main/cpp/oreslang/runtime/ActorOwnedI64Heap.c"))
        .arg(&object)
        .arg("-o")
        .arg(&executable)
        .output()
        .map_err(|e| e.to_string())?;
    require(
        linked.status.success(),
        format!("cannot link real .atto actor and native heap: {}", stderr(&linked)),
    )?;
    let observed = Command::new(&executable)
        .env_clear()
        .env("ASAN_OPTIONS", "detect_leaks=1:halt_on_error=1:exitcode=86")
        .env("UBSAN_OPTIONS", "halt_on_error=1:exitcode=86")
        .output()
        .map_err(|e| format!("cannot execute instrumented actor guest: {e}"))?;
    require(
        observed.status.code() == Some(0)
            && observed.stdout.is_empty()
            && observed.stderr.is_empty(),
        format!(
            "instrumented .atto actor guest failed: status={:?}, stderr={}",
            observed.status.code(),
            stderr(&observed)
        ),
    )?;

    // Unsupported guest calls must never produce partial IR, even when the
    // valid actor fixture has already compiled and linked correctly.
    let malformed = temp.path.join("unsupported.atto");
    let text = String::from_utf8(source).map_err(|e| e.to_string())?;
    require(text.matches("return total;").count() == 1, "mutation site drift")?;
    fs::write(
        &malformed,
        text.replace("return total;", "return unknown_actor_operation(total);"),
    )
    .map_err(|e| e.to_string())?;
    let rejected = run(
        compiler,
        [OsStr::new("--target=host"), malformed.as_os_str()],
    )?;
    require(
        !rejected.status.success()
                && rejected.status.code().is_some()
            && rejected.stdout.is_empty()
            && !rejected.stderr.is_empty(),
        "unsupported .atto actor operation emitted LLVM IR or lacked a compiler diagnostic",
    )?;

    println!("ATTO_LLVM_COMPILER_BACKED_ACTOR_ASAN_UBSAN_PASS guest=atto native_heap=real negative=1");
    Ok(())
}
