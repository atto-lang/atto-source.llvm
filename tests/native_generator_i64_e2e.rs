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
            "ores-generator-source-{}-{stamp}", std::process::id()));
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
        .expect("usage: generator-e2e COMPILER CLANG"))
        .canonicalize().expect("compiler");
    let clang = PathBuf::from(args.next().expect("missing clang"))
        .canonicalize().expect("clang");
    let dir = Temp::new();

    let source = r#"pub gen* values(): Iterator<int> {
  yield 1;
  yield -9223372036854775808;
  yield 9223372036854775807;
  return;
}
"#;
    let output = compile(&compiler, &dir.0, "values.ores", source);
    assert!(output.status.success(),
        "generator source compile failed: {}", String::from_utf8_lossy(&output.stderr));
    let ir = String::from_utf8_lossy(&output.stdout);
    for needle in [
        "define i32 @oreslang_rt_gen_values_init(",
        "define i32 @oreslang_rt_gen_values_next(",
        "define i32 @oreslang_rt_gen_values_close(",
    ] {
        assert!(ir.contains(needle), "missing generator IR marker {needle}");
    }
    assert!(!ir.contains("define i32 @main("), "generator-only source invented OS main");

    let ll = dir.0.join("generator.ll");
    let harness = dir.0.join("harness.c");
    let exe = dir.0.join(if cfg!(windows) { "generator.exe" } else { "generator" });
    fs::write(&ll, &output.stdout).expect("write IR");
    fs::write(&harness, r#"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int oreslang_rt_gen_values_init(int64_t*);
extern int oreslang_rt_gen_values_next(int64_t*, int64_t*);
extern int oreslang_rt_gen_values_close(int64_t*);
static void check(int ok, const char* message) {
    if (!ok) { fputs(message, stderr); fputc('\n', stderr); abort(); }
}
int main(void) {
    int64_t frame[2] = {0,0};
    int64_t value = 99;
    check(oreslang_rt_gen_values_init(NULL)==-1, "null init accepted");
    check(oreslang_rt_gen_values_next(NULL,&value)==-1, "null frame next accepted");
    check(oreslang_rt_gen_values_next(frame,NULL)==-1, "null output next accepted");
    check(oreslang_rt_gen_values_close(NULL)==-1, "null close accepted");
    check(oreslang_rt_gen_values_next(frame,&value)==-1, "uninitialized next accepted");
    check(value==99 && frame[0]==0 && frame[1]==0, "invalid next mutated state");
    check(oreslang_rt_gen_values_close(frame)==-1, "uninitialized close accepted");
    int64_t forged[2] = {1, 99};
    value = 77;
    check(oreslang_rt_gen_values_init(forged)==-1, "forged active yield index accepted by init");
    check(oreslang_rt_gen_values_next(forged,&value)==-1, "forged yield index accepted as DONE");
    check(forged[0]==1 && forged[1]==99 && value==77, "forged index rejection mutated frame/output");
    int64_t forged_state[2] = {99, 0};
    check(oreslang_rt_gen_values_init(forged_state)==-1, "unknown generator state accepted by init");
    check(forged_state[0]==99 && forged_state[1]==0, "unknown-state rejection mutated frame");
    int64_t forged_done[2] = {2, 1};
    check(oreslang_rt_gen_values_init(forged_done)==-1, "forged terminal index accepted by init");
    check(oreslang_rt_gen_values_next(forged_done,&value)==-1, "forged terminal index accepted as DONE");
    check(oreslang_rt_gen_values_close(forged_done)==-1, "forged terminal index accepted by close");
    check(forged_done[0]==2 && forged_done[1]==1, "forged terminal rejection mutated frame");
    check(oreslang_rt_gen_values_init(frame)==1, "init failed");
    check(oreslang_rt_gen_values_init(frame)==0, "double init accepted");
    const int64_t expected_values[3] = {1, INT64_MIN, INT64_MAX};
    for (size_t i=0; i<3; ++i) {
        value = -1;
        check(oreslang_rt_gen_values_next(frame,&value)==1, "yield missing");
        check(value==expected_values[i], "yield value/order mismatch");
    }
    check(oreslang_rt_gen_values_next(frame,&value)==0, "missing DONE");
    check(frame[0]==2 && frame[1]==0, "DONE did not retire frame");
    check(oreslang_rt_gen_values_next(frame,&value)==0, "DONE not sticky");
    check(oreslang_rt_gen_values_close(frame)==0, "close after DONE changed state");
    check(oreslang_rt_gen_values_init(frame)==0, "terminal frame resurrected");

    int64_t early[2] = {0,0};
    check(oreslang_rt_gen_values_init(early)==1, "early frame init failed");
    check(oreslang_rt_gen_values_next(early,&value)==1 && value==1, "early first yield failed");
    check(oreslang_rt_gen_values_close(early)==1, "active close failed");
    check(early[0]==2 && early[1]==0, "close did not retire/zero index");
    check(oreslang_rt_gen_values_next(early,&value)==0, "closed frame resumed");
    check(oreslang_rt_gen_values_close(early)==0, "double close accepted");
    puts("NATIVE_SYNC_GENERATOR_FRAME_PASS");
    return 0;
}
"#).expect("write harness");

    let build = Command::new(&clang)
        .arg("-O2").arg("-std=c11")
        .arg("-Wall").arg("-Wextra").arg("-Werror").arg("-pedantic")
        .arg(&harness).arg("-x").arg("ir").arg(&ll)
        .arg("-o").arg(&exe).current_dir(&dir.0)
        .output().expect("run clang");
    assert!(build.status.success(),
        "generator native link failed: {}", String::from_utf8_lossy(&build.stderr));
    let run = Command::new(&exe).current_dir(&dir.0)
        .output().expect("run generator native binary");
    assert!(run.status.success(),
        "generator native execution failed: {}", String::from_utf8_lossy(&run.stderr));
    assert!(String::from_utf8_lossy(&run.stdout)
        .contains("NATIVE_SYNC_GENERATOR_FRAME_PASS"));

    let alias_source = r#"pub generator* alias_values(): Iterator<int> {
  yield 7;
  return;
}
"#;
    let alias = compile(&compiler, &dir.0, "generator-star-alias.ores", alias_source);
    assert!(alias.status.success(),
        "generator* compatibility alias compile failed: {}",
        String::from_utf8_lossy(&alias.stderr));
    assert!(String::from_utf8_lossy(&alias.stdout)
        .contains("define i32 @oreslang_rt_gen_alias_values_init("));

    rejected(&compiler, &dir.0, "starless-generator.ores",
        "pub generator values(): Iterator<int> { yield 1; return; }\n",
        "bare native generator declaration requires 'generator*'");

    rejected(&compiler, &dir.0, "duplicate-generator.ores",
        "gen* values(): Iterator<int> { yield 1; return; }\n\
gen* values(): Iterator<int> { yield 2; return; }\n",
        "duplicate native generator name/arity");

    rejected(&compiler, &dir.0, "generator-function-collision.ores",
        "fnc values(): int { return 0; }\n\
gen* values(): Iterator<int> { yield 1; return; }\n",
        "duplicate generator/function/class symbol 'values'");

    let mut too_many = String::new();
    for i in 0..513 {
        too_many.push_str(&format!(
            "gen* g{i}(): Iterator<int> {{ yield {i}; return; }}\n"));
    }
    rejected(&compiler, &dir.0, "too-many-generators.ores", &too_many,
        "native generator declaration limit exceeded (512)");

    rejected(&compiler, &dir.0, "async.ores",
        "async gen* values(): AsyncIterator<int> { yield 1; return; }\n",
        "async gen* requires scheduler-owned async iterator continuation lowering");
    rejected(&compiler, &dir.0, "suspend.ores",
        "suspend gen* values(): AsyncIterator<int> { yield 1; return; }\n",
        "suspend gen* requires scheduler-owned async iterator continuation lowering");
    rejected(&compiler, &dir.0, "delegate.ores",
        "gen* values(): Iterator<int> { yield* other; return; }\n",
        "yield* requires delegated iterator ownership and close lowering");
    rejected(&compiler, &dir.0, "return-value.ores",
        "gen* values(): Iterator<int> { return 1; }\n",
        "generator return cannot carry a value");
    rejected(&compiler, &dir.0, "parameter.ores",
        "gen* values(int n): Iterator<int> { yield 1; return; }\n",
        "sync generator v1 is zero-argument");

    rejected(&compiler, &dir.0, "positive-overflow.ores",
        "gen* values(): Iterator<int> { yield 9223372036854775808; return; }\n",
        "integer literal overflows signed 64-bit result");
    rejected(&compiler, &dir.0, "negative-overflow.ores",
        "gen* values(): Iterator<int> { yield -9223372036854775809; return; }\n",
        "integer literal overflows signed 64-bit result");

    println!("PASS: native sync gen* source -> LLVM frame -> Clang execution; async/suspend/delegation fail closed");
}
