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
    check(oreslang_rt_gen_values_next(frame,frame)==-1,
          "next accepted output alias of frame state");
    check(oreslang_rt_gen_values_next(frame,frame+1)==-1,
          "next accepted output alias of frame index");
    check(frame[0]==1 && frame[1]==0, "alias rejection mutated frame");
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
        // Source IR is target-neutral; Clang assigns the host triple when
        // linking this native execution test, including on Apple Silicon.
        .arg("-Wno-override-module")
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

    // Pure same-file yield* is resolved into a bounded native yield sequence
    // without an escaping iterator loan, producer task or heap frame.
    let delegated_source = r#"pub gen* inner(): Iterator<int> {
  yield 20;
  yield 22;
  return;
}
pub gen* outer(): Iterator<int> {
  yield 0;
  yield* inner();
  yield 1;
  return;
}
pub gen* forwarded(): Iterator<int> {
  yield* outer();
  return;
}
"#;
    let delegated = compile(
        &compiler, &dir.0, "pure-delegation.ores", delegated_source);
    assert!(delegated.status.success(),
        "bounded yield* compile failed: {}",
        String::from_utf8_lossy(&delegated.stderr));
    let delegated_ir = String::from_utf8_lossy(&delegated.stdout);
    assert!(delegated_ir.contains("define i32 @oreslang_rt_gen_forwarded_next("),
        "forwarded native generator ABI missing");
    let delegated_ll = dir.0.join("pure-delegation.ll");
    let delegated_c = dir.0.join("pure-delegation-harness.c");
    let delegated_exe = dir.0.join(if cfg!(windows) {
        "pure-delegation-harness.exe"
    } else {
        "pure-delegation-harness"
    });
    fs::write(&delegated_ll, &delegated.stdout).expect("write delegated IR");
    fs::write(&delegated_c, r#"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
extern int oreslang_rt_gen_forwarded_init(int64_t*);
extern int oreslang_rt_gen_forwarded_next(int64_t*, int64_t*);
extern int oreslang_rt_gen_forwarded_close(int64_t*);
static void check(int ok, const char* msg) {
    if (!ok) { fputs(msg, stderr); fputc('\n', stderr); abort(); }
}
int main(void) {
    int64_t first[2] = {0, 0}, second[2] = {0, 0}, v = 0;
    const int64_t expected[4] = {0, 20, 22, 1};
    check(oreslang_rt_gen_forwarded_init(first) == 1, "init first");
    check(oreslang_rt_gen_forwarded_init(second) == 1, "init second");
    for (size_t i = 0; i < 4; ++i) {
        check(oreslang_rt_gen_forwarded_next(first, &v) == 1, "first yield");
        check(v == expected[i], "delegated ordering");
        if (i == 0) {
            check(oreslang_rt_gen_forwarded_next(second, &v) == 1,
                  "second activation yield");
            check(v == 0, "second activation independence");
            check(oreslang_rt_gen_forwarded_close(second) == 1, "early close");
            check(oreslang_rt_gen_forwarded_next(second, &v) == 0,
                  "early-closed delegate revived");
        }
    }
    check(oreslang_rt_gen_forwarded_next(first, &v) == 0, "missing DONE");
    check(oreslang_rt_gen_forwarded_next(first, &v) == 0, "DONE not sticky");
    check(oreslang_rt_gen_forwarded_close(first) == 0, "close after DONE");
    puts("NATIVE_PURE_DELEGATION_PASS");
    return 0;
}
"#).expect("write delegated harness");
    let delegate_build = Command::new(&clang)
        .arg("-O2").arg("-std=c11")
        .arg("-Wall").arg("-Wextra").arg("-Werror").arg("-pedantic")
        .arg("-Wno-override-module")
        .arg(&delegated_c).arg("-x").arg("ir").arg(&delegated_ll)
        .arg("-o").arg(&delegated_exe).current_dir(&dir.0)
        .output().expect("build delegated native generator");
    assert!(delegate_build.status.success(),
        "delegated native link failed: {}",
        String::from_utf8_lossy(&delegate_build.stderr));
    let delegate_run = Command::new(&delegated_exe).current_dir(&dir.0)
        .output().expect("execute delegated native generator");
    assert!(delegate_run.status.success(),
        "delegated native execution failed: {}",
        String::from_utf8_lossy(&delegate_run.stderr));
    assert!(String::from_utf8_lossy(&delegate_run.stdout)
        .contains("NATIVE_PURE_DELEGATION_PASS"));

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

    let arity_overload_source =
        "fnc values(int x): int { return x; }\n\
gen* values(): Iterator<int> { yield 1; return; }\n";
    let arity_overload = compile(
        &compiler, &dir.0, "generator-function-arity-overload.ores", arity_overload_source);
    assert!(arity_overload.status.success(),
        "different value arities must coexist: {}",
        String::from_utf8_lossy(&arity_overload.stderr));
    let arity_overload_ir = String::from_utf8_lossy(&arity_overload.stdout);
    assert!(arity_overload_ir.contains("define i64 @values("),
        "ordinary arity-1 function missing from mixed generator program");
    assert!(arity_overload_ir.contains("define internal i32 @oreslang_rt_gen_values_init("),
        "private zero-arity generator ABI missing from mixed generator program");
    assert!(!arity_overload_ir.contains("define i32 @oreslang_rt_gen_values_init("),
        "private generator ABI must not be exported");

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
    rejected(&compiler, &dir.0, "delegate-not-call.ores",
        "gen* values(): Iterator<int> { yield* other; return; }\n",
        "expected '('");
    rejected(&compiler, &dir.0, "delegate-missing.ores",
        "gen* values(): Iterator<int> { yield* missing(); return; }\n",
        "native generator yield* target is not a same-file literal generator");
    rejected(&compiler, &dir.0, "delegate-with-args.ores",
        "gen* values(): Iterator<int> { yield* other(2); return; }\n",
        "expected ')'");
    rejected(&compiler, &dir.0, "delegate-cycle.ores",
        concat!(
            "gen* a(): Iterator<int> { yield* b(); return; }\n",
            "gen* b(): Iterator<int> { yield* a(); return; }\n",
        ),
        "native generator yield* cycle");
    let mut expanded_overflow = String::from("gen* inner(): Iterator<int> {\n");
    for _ in 0..2049 { expanded_overflow.push_str(" yield 1;\n"); }
    expanded_overflow.push_str(
        "}\ngen* outer(): Iterator<int> { yield* inner(); yield* inner(); yield 1; return; }\n");
    rejected(&compiler, &dir.0, "delegate-expanded-overflow.ores",
        &expanded_overflow, "exceeds 4096 yielded elements after yield*");
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

    println!("PASS: native sync gen* and pure yield* -> LLVM frame -> Clang execution; async/suspend/impure delegation fail closed");
}
