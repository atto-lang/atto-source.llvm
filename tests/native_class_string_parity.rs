//! Execute real LLVM-generated machine code, optionally comparing Java output.
use std::{
    env, fs,
    io::Write,
    path::{Path, PathBuf},
    process::{Command, Output, Stdio},
    thread,
    time::{Duration, Instant, SystemTime, UNIX_EPOCH},
};
struct Scratch(PathBuf);
impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}
fn run(command: &mut Command) -> Output {
    command
        .output()
        .unwrap_or_else(|error| panic!("cannot run {command:?}: {error}"))
}
const JAVA_LLVM_STRICT_NINE: [&str; 9] = [
    "exact-user-program",
    "string-content-equality",
    "string-equality-embedded-nul-and-unicode",
    "http-string-routing-primitives",
    "source-effect-order",
    "parameters-and-return",
    "lazy-conditional",
    "escaped-unicode",
    "empty-and-embedded-nul",
];
fn valid_git_revision(value: &str) -> bool {
    value.len() == 40 && value.bytes().all(|b| b.is_ascii_hexdigit())
}

// Binary-safe stdout evidence; unlike UTF-8 conversion, this also retains
// embedded NUL and exact source output bytes.
fn hex_encode(bytes: &[u8]) -> String {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut out = Vec::with_capacity(bytes.len() * 2);
    for byte in bytes {
        out.push(DIGITS[usize::from(*byte >> 4)]);
        out.push(DIGITS[usize::from(*byte & 0x0f)]);
    }
    String::from_utf8(out).expect("hex alphabet is ASCII")
}

fn evidence_admission(
    require_java: bool,
    java: Option<&str>,
    java_revision: Option<&str>,
    llvm_revision: Option<&str>,
    report: Option<&str>,
) -> Result<(), &'static str> {
    if !require_java {
        if report.is_some() || java_revision.is_some() || llvm_revision.is_some() {
            return Err("revision-stamped evidence requires --require-java");
        }
        return Ok(());
    }
    if java.map_or(true, |value| value.trim().is_empty()) {
        return Err("--require-java requires a nonempty --java-command; missing Java must fail closed");
    }
    if !java_revision.is_some_and(valid_git_revision) {
        return Err("--require-java requires a 40-hex --java-revision");
    }
    if !llvm_revision.is_some_and(valid_git_revision) {
        return Err("--require-java requires a 40-hex --llvm-revision");
    }
    if report.map_or(true, |value| value.trim().is_empty()) {
        return Err("--require-java requires a nonempty --report for a durable execution receipt");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{evidence_admission, hex_encode, valid_git_revision};

    const SHA: &str = "0123456789abcdef0123456789abcdef01234567";

    #[test]
    fn rejects_missing_reference_or_provenance() {
        assert!(evidence_admission(true, None, Some(SHA), Some(SHA), Some("r.json")).is_err());
        assert!(evidence_admission(true, Some("java"), None, Some(SHA), Some("r.json")).is_err());
        assert!(evidence_admission(true, Some("java"), Some(SHA), None, Some("r.json")).is_err());
        assert!(evidence_admission(true, Some("java"), Some(SHA), Some(SHA), None).is_err());
        assert!(evidence_admission(false, None, None, None, Some("r.json")).is_err());
        assert!(evidence_admission(false, None, Some(SHA), Some(SHA), None).is_err());
        assert!(evidence_admission(true, Some(""), Some(SHA), Some(SHA), Some("r.json")).is_err());
        assert!(evidence_admission(true, Some("java"), Some(SHA), Some(SHA), Some("")).is_err());
    }

    #[test]
    fn preserves_binary_stdout_evidence() {
        assert_eq!(hex_encode(b"a\x00b"), "610062");
        assert_eq!(hex_encode("é".as_bytes()), "c3a9");
    }

    #[test]
    fn admits_only_pinned_strict_comparison() {
        assert!(valid_git_revision(SHA));
        assert!(!valid_git_revision("main"));
        assert!(!valid_git_revision("g".repeat(40).as_str()));
        assert!(evidence_admission(true, Some("java"), Some(SHA), Some(SHA), Some("r.json")).is_ok());
        assert!(evidence_admission(false, None, None, None, None).is_ok());
    }
}

// Negative admission is adversarial: a broken O_NONBLOCK/fstat guard must
// fail the test quickly rather than consume an entire macOS/Linux CI job.
// Redirect both streams to files/null to avoid pipe-buffer deadlocks.
#[cfg(unix)]
fn run_bounded_no_ir(command: &mut Command, scratch: &Path, label: &str) {
    let stdout_path = scratch.join("bounded-negative-stdout");
    let stdout = fs::File::create(&stdout_path).expect("create negative output file");
    let mut child = command
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::null())
        .spawn()
        .unwrap_or_else(|error| panic!("{label}: cannot spawn compiler: {error}"));
    let deadline = Instant::now() + Duration::from_secs(8);
    let status = loop {
        match child.try_wait().expect("poll child") {
            Some(status) => break status,
            None if Instant::now() >= deadline => {
                let _ = child.kill();
                let _ = child.wait();
                panic!("{label}: compiler blocked beyond eight seconds");
            }
            None => thread::sleep(Duration::from_millis(10)),
        }
    };
    assert_eq!(status.code(), Some(1), "{label}: expected a rejected source");
    assert_eq!(fs::metadata(&stdout_path).unwrap().len(), 0,
               "{label}: compiler emitted partial IR");
    fs::remove_file(&stdout_path).unwrap();
}

// Exercise the generated function boundary, including a callee that never
// reads its parameter. Invalid descriptors must abort before any payload use.
fn check_sum_boundary(compiler: &str, clang: &str, runtime: &str, scratch: &std::path::Path, sanitize: bool) {
    use std::os::unix::process::ExitStatusExt;
    let source = scratch.join("sum-boundary.ores");
    fs::write(&source, "fnc validate(Option<int> item):int{return 42;}fnc inspect(Option<int> item):int{return item.unwrap_or(7);}pub fnc main():int{return 0;}").unwrap();
    let generated = run(Command::new(compiler).arg(&source));
    assert!(generated.status.success(), "sum boundary IR: {}", String::from_utf8_lossy(&generated.stderr));
    let ir = String::from_utf8(generated.stdout).unwrap().replace("@main(", "@unused_os_main(");
    let ir = if sanitize { ir.lines().map(|line| if line.starts_with("define ") {
        format!("{} sanitize_address {{\n", line.strip_suffix('{').unwrap())
    } else { format!("{line}\n") }).collect::<String>() } else { ir };
    let path = scratch.join("sum-boundary.ll");
    fs::write(&path, ir).unwrap();
    let harness = scratch.join("sum-boundary.c");
    fs::write(&harness, r#"#include "oreslang/runtime/NativeRecordRegion.h"
#include <stdint.h>
#include <stdlib.h>
extern int64_t validate(int64_t);
extern int64_t inspect(int64_t);
int main(int argc,char** argv) {
  if(argc!=2 || ores_record_region_begin()!=0) return 90;
  int mode=atoi(argv[1]);
  const int64_t fields[3]={mode==1?0:mode==5?-1:mode==6?2:1,mode==1?INT64_MAX:42,0};
  int64_t value=ores_record_new(mode==3?2:1,mode==4?1:mode==8?3:2,fields);
  if(mode==2){if(ores_record_region_end()!=0||ores_record_region_begin()!=0)return 91;}
  if(mode==7)value=0;
  if(validate(value)!=42 || inspect(value)!=(mode==1?7:42))return 92;
  return ores_record_region_end()!=0?93:0;
}
"#).unwrap();
    let binary = scratch.join("sum-boundary");
    let runtime_root = PathBuf::from(runtime).parent().unwrap().to_path_buf();
    let mut link = Command::new(clang);
    link.args(["-O1", "-g"]);
    if sanitize { link.args(["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer"]); }
    let linked = run(link.args(["-x", "ir"]).arg(path).args(["-x", "c", "-std=c11", "-I"])
        .arg(runtime_root.parent().unwrap().parent().unwrap()).arg(harness)
        .arg(runtime_root.join("NativeRecordRegion.c")).args(["-pthread", "-o"]).arg(&binary));
    assert!(linked.status.success(), "sum boundary link: {}", String::from_utf8_lossy(&linked.stderr));
    for mode in 0..9 {
        let output = run(Command::new(&binary).arg(mode.to_string()));
        if mode < 2 { assert!(output.status.success(), "sum boundary valid case {mode}"); }
        else { assert_eq!(output.status.signal(), Some(6), "sum boundary invalid case {mode}: {}", String::from_utf8_lossy(&output.stderr)); }
        assert!(output.stdout.is_empty() && output.stderr.is_empty(), "sum boundary diagnostic: {}", String::from_utf8_lossy(&output.stderr));
    }
    println!("PASS generated sum ABI: present/absent payloads, stale generation, schema, arity, invalid tags and null rejection");
}
fn main() {
    let (mut compiler, mut clang, mut runtime, mut java) = (None, None, None, None);
    let mut sanitize = false;
    let mut aot_driver = None;
    let mut require_java = false;
    let mut java_args = Vec::<String>::new();
    let (mut java_revision, mut llvm_revision, mut receipt_path) = (None, None, None);
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--compiler" => compiler = args.next(),
            "--clang" => clang = args.next(),
            "--runtime" => runtime = args.next(),
            "--java-command" => java = args.next(),
            "--java-arg" => java_args.push(args.next().expect("--java-arg needs a value")),
            "--require-java" => require_java = true,
            "--java-revision" => java_revision = args.next(),
            "--llvm-revision" => llvm_revision = args.next(),
            "--report" => receipt_path = args.next(),
            "--aot-driver" => aot_driver = args.next(),
            "--sanitize" => sanitize = true,
            _ => panic!("unknown argument {arg}"),
        }
    }
    if let Err(error) = evidence_admission(
        require_java,
        java.as_deref(),
        java_revision.as_deref(),
        llvm_revision.as_deref(),
        receipt_path.as_deref(),
    ) {
        panic!("{error}");
    }
    let (compiler, clang, runtime) = (
        compiler.expect("--compiler"),
        clang.expect("--clang"),
        runtime.expect("--runtime"),
    );
    let scratch = Scratch(env::temp_dir().join(format!(
            "ores-class-string-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        )));
    fs::create_dir(&scratch.0).unwrap();
    let exact = include_str!("fixtures/native-class-string/main.ores");
    let foo = include_str!("fixtures/native-class-string/main-1.ores");
    let positives = [
        ("atto-nats-split-literal-empty-tokens",
         r#"pub fnc main():int{
            val String subject="orders..created.";
            val parts=subject.split_literal(".");
            val String empty="";
            val empties=empty.split_literal(".");
            val String nul="a\u0000b";
            val binary=nul.split_literal("\u0000");
            val String longsep="a::b::::";
            val multi=longsep.split_literal("::");
            return parts.size==4&&parts.get(0)=="orders"&&
                parts.get(1)==""&&parts.get(2)=="created"&&
                parts.get(3)==""&&empties.size==1&&empties.get(0)==""&&
                binary.size==2&&binary.get(0)=="a"&&binary.get(1)=="b"&&
                multi.size==4&&multi.get(0)=="a"&&multi.get(1)=="b"&&
                multi.get(2)==""&&multi.get(3)==""?0:1;
         }"#, ""),
        ("ordered-array-moves-through-owning-return", "define class Box as pub val Array<int> values;end fnc touch():void{return;}fnc build():Box{touch();val Array<int> values=new Array<int>();values.add(42);return new Box(values);}pub fnc main():int{val Box box=build();return box.values.get(0)==42?0:1;}", ""),
        ("ordered-array-sibling-scopes", "fnc touch():void{return;}pub fnc main():int{touch();if true;then val Array<int> values=new Array<int>();values.add(1);fi if true;then val Array<String> values=new Array<String>();values.add(\"ok\");if values.get(0)!=\"ok\";then return 1;fi fi return 0;}", ""),
        ("ordered-record-array-loop", "define class EntryValue as pub val int value;end fnc touch():void{return;}pub fnc main():int{touch();let int total=0;for(let int i=0;i<100;i=i+1){val Array<EntryValue> values=new Array<EntryValue>();values.add(new EntryValue(i));val EntryValue entry=values.get(0);total=total+entry.value;}return total==4950?0:1;}", ""),

        ("ordered-array-scopes-loop-and-early-return", include_str!("fixtures/native-class-string/ordered-array-locals.ores"), ""),
        ("checked-copy-option-guest", include_str!("fixtures/native-class-string/copy-option-v1.atto"), "7:42:true:true:true:true:9"),
        ("ordered-val-keeps-exclusive-referent-capability", "fnc touch():void{return;}fnc observe(borrow mut int value):int{return 42;}pub fnc main():int{touch();val int value=42;return observe(rt mut borrow value)==42?0:1;}", ""),
        ("ordered-initial-and-later-mutable-slots", "fnc touch():void{return;}pub fnc main():int{let int initial=1;touch();let int later=2;initial=initial+later;return initial==3?0:1;}", ""),
        ("ordered-routing-counters-and-skipped-initializers", include_str!("fixtures/native-class-string/ordered-routing-locals.ores"), "enter;enter;initialize;finish;done;"),
        ("ordered-root-scalar-lives-through-defer", r#"fnc touch():void{stdio.print("enter;");return;}fnc observe(int value):void{stdio.print(value==42?"final;":"early;");return;}
          fnc run(bool early):int{touch();let int value=1;defer observe(value);if early;then return value;fi value=42;return value;}
          pub fnc main():int{val int later=run(false);val int first=run(true);return later==42&&first==1?0:1;}"#, "enter;final;enter;early;"),
        ("ordered-root-conditional-scalar-write", r#"fnc touch():void{return;}
          pub fnc main():int{touch();let int value=42;value=true?value+1:value+2;return value==43?0:1;}"#, ""),
        ("ordered-root-sum-reassignment", r#"fnc touch():void{return;}pub fnc main():int{touch();let Option<int> selected=None;selected=Some(42);let Result<String,bool> checked=Err(false);checked=Ok("yes");return selected.unwrap()==42&&checked.unwrap()=="yes"?0:1;}"#, ""),
        ("ordered-root-string-and-bool-reassignment", r#"fnc touch():void{return;}pub fnc main():int{touch();let String label="before";let bool enabled=false;label="after";enabled=true;return enabled&&label=="after"?0:1;}"#, ""),
        ("ordered-immutable-record-result", r#"define class Info as pub val int code;pub get():int{return self.code;}end fnc touch():void{return;}fnc produce():Info{touch();val Info value=new Info(42);return value;}pub fnc main():int{val Info result=produce();return result.get()==42?0:1;}"#, ""),
        ("ordered-local-scopes-do-not-leak", r#"fnc touch():void{return;}pub fnc main():int{touch();let int total=0;for(let int index=0;index<2;index=index+1){let int local=index;local=local+1;total=total+local;}if total==3;then let int local=39;total=total+local;fi return total==42?0:1;}"#, ""),
        ("contextual-pattern-function-and-parameter", "fnc pattern(int pattern):int{return pattern;}pub fnc main():int{return pattern(42)==42?0:1;}", ""),
        ("sum-mixed-guard-source-order", "fnc choose(bool first):Option<int>{if first;then return Some(1);fi if(true){return Some(2);}fi return None;}pub fnc main():int{return choose(true).unwrap()==1&&choose(false).unwrap()==2?0:1;}", ""),
        ("adt-conditional-option-tagged-arms",
            include_str!("fixtures/adt-conditional-v1/option-positive.atto"), ""),
        ("adt-conditional-result-tagged-arms",
            include_str!("fixtures/adt-conditional-v1/result-positive.atto"), ""),
        ("sum-result-schema-matrix", r#"pub routine main():int{val Result<int,int> ii=Ok(0);val Result<int,bool> ib=Err(false);val Result<bool,int> bi=Ok(false);val Result<String,bool> sb=Ok("");val Result<String,String> ss=Err("");return ii.is_ok()&&ii.unwrap()==0&&ib.is_err()&&bi.is_ok()&&!bi.unwrap()&&sb.is_ok()&&sb.unwrap()==""&&ss.is_err()?0:1;}"#, ""),
        ("imported-canonical-sum-identity", r#"import module sums from './sums.ores';pub routine main():int{val Option<String> item=sums.choose(true);val Result<bool,String> result=sums.checked(false);return item.unwrap()=="module"&&result.is_err()?0:1;}"#, ""),
        ("actual-routing-option-capture", include_str!("fixtures/native-class-string/routing-option-capture.ores"), ""),
        ("sum-zero-false-empty-distinguish-absence", r#"pub routine main():int{val Option<int> zero=Some(0);val Option<bool> no=Some(false);val Option<String> empty=Some("");val Option<int> absent=None;val Result<bool,bool> ok=Ok(false);val Result<bool,bool> err=Err(false);return zero.is_some()&&zero.unwrap()==0&&no.is_some()&&!no.unwrap()&&empty.is_some()&&empty.unwrap()==""&&absent.is_none()&&ok.is_ok()&&!ok.unwrap()&&err.is_err()?0:1;}"#, ""),
        ("sum-typed-parameters-and-returns", r#"fnc relay(Option<int> item):Option<int>{return item;}fnc observe(borrow Option<int> item):int{return item.unwrap_or(7);}fnc result(bool ok):Result<String,int>{return ok?Ok("yes"):Err(0);}pub routine main():int{val Option<int> item=Some(42);val Option<int> copy=relay(item);val Result<String,int> good=result(true);val Result<String,int> bad=result(false);return observe(rt borrow copy)==42&&item.unwrap()==42&&good.unwrap()=="yes"&&bad.unwrap_or("no")=="no"?0:1;}"#, ""),
        ("sum-fallback-is-eager", r#"fnc fallback():int{stdio.print("fallback;");return 17;}pub routine main():int{val Option<int> yes=Some(42);val Option<int> no=None;val Result<int,String> good=Ok(29);val Result<int,String> bad=Err("missing");return yes.unwrap_or(fallback())==42&&no.unwrap_or(fallback())==17&&good.unwrap_or(fallback())==29&&bad.unwrap_or(fallback())==17?0:1;}"#, "fallback;fallback;fallback;fallback;"),
        ("sum-fields-and-source-ordered-guards", r#"define class Holder as pub let Option<int> value;pub pattern():Option<int>{return self.value;}pub replace(mut self)(Option<int> value):void{self.value=value;return;}end fnc choose_sum(bool early):Option<int>{if early;then return None;fi for(let int index=0;index<2;index=index+1){if index==1;then return Some(index);fi}return None;}pub routine main():int{val Holder pattern=new Holder(None);val Option<int> chosen=choose_sum(false);pattern.replace(Some(42));return pattern.pattern().unwrap()==42&&choose_sum(true).is_none()&&chosen.unwrap()==1?0:1;}"#, ""),
        ("sum-fields-and-contextual-construction", r#"define class Holder as pub val Option<int> missing;pub val Option<bool> available;pub val Result<String,int> result;end pub routine main():int{val Holder holder=new Holder(None,Some(false),Err(0));val Option<int> inferred=Some(9);return holder.missing.is_none()&&holder.available.is_some()&&!holder.available.unwrap()&&holder.result.is_err()&&inferred.unwrap()==9?0:1;}"#, ""),
        ("nested-multiple-children-offsets-and-return-regions", r#"define class Data as pub val Array<int> values;pub val String label;pub read():int{return self.values.get(0);}end
          define class Pair as pub val int leading;pub val Data left;pub val int gap;pub val Data right;end
          fnc make_data(int n,String label):Data{val Array<int> values=new Array<int>();values.add(n);return new Data(values,label);}
          fnc make_pair():Pair{val Data left=make_data(13,"left");val Data right=make_data(29,"right");return new Pair(7,left,11,right);}
          fnc add_data(borrow Data a,borrow Data b):int{return a.read()+b.read();}
          pub fnc main():int{val Pair pair=make_pair();return pair.leading==7&&pair.gap==11&&pair.left.label=="left"&&pair.right.label=="right"&&add_data(rt borrow pair.left,rt borrow pair.right)==42?0:1;}"#, ""),
        ("imported-nested-owned-capture", r#"import module captures from './captures.ores';pub routine main():int{return captures.verify_capture();}"#, "id\n"),
        ("nested-immutable-record-array-iteration", r#"define class Leaf as pub val int code;pub read():int{return self.code;}end define class Outer as pub val Leaf leaf;end
          define class Counter as pub let int sum=0;end
          pub fnc main():int{val Array<Outer> values=new Array<Outer>();values.add(new Outer(new Leaf(7)));values.add(new Outer(new Leaf(11)));let mut Counter count=new Counter();for(const item of values){count.sum=count.sum+item.leaf.read();}return count.sum==18?0:1;}"#, ""),
        ("nested-owned-capture-return-and-move", include_str!("fixtures/native-class-string/nested-owned-capture.ores"), "id\n"),
        ("nested-constructor-result-and-grandchild", r#"define class Data as pub val Array<int> values;pub val String label;pub read():int{return self.values.get(0);}end
          define class Envelope as pub val Data data;pub val int tag;pub read():int{return self.data.read();}end
          define class Root as pub val Envelope envelope;pub read():int{return self.envelope.read();}end
          fnc make_envelope():Envelope{val Array<int> values=new Array<int>();values.add(42);val Data data=new Data(values,"nested");return new Envelope(data,7);}
          fnc make_root():Root{val Envelope envelope=make_envelope();return new Root(envelope);}
          fnc read_data(borrow Data data):int{return data.read();}
          pub routine main():int{val Root root=make_root();return root.read()==42&&read_data(rt borrow root.envelope.data)==42&&root.envelope.tag==7&&root.envelope.data.label=="nested"?0:1;}"#, ""),
        ("nested-projection-exclusively-mutates-array", r#"define class Data as pub val Array<int> values;pub edit(mut self)(int value):void{self.values.set(0,value);return;}end
          define class Outer as pub val Data data;end
          pub routine main():int{val Array<int> values=new Array<int>();values.add(1);val Data data=new Data(values);val Outer outer=new Outer(data);outer.data.edit(42);return outer.data.values.get(0)==42?0:1;}"#, ""),
        ("nested-immutable-scalar-result", r#"define class Leaf as pub val String name;pub read():String{return self.name;}end
          define class Outer as pub val Leaf leaf;pub read():String{return self.leaf.read();}end
          fnc make_outer():Outer{val Leaf leaf=new Leaf("value");return new Outer(leaf);}
          pub routine main():int{val Outer outer=make_outer();return outer.read()=="value"&&outer.leaf.name=="value"?0:1;}"#, ""),
        ("imported-module-routing-validation", r#"import module paths from './paths.ores';pub routine main():int{return paths.verify_paths();}"#, ""),
        ("void-structured-return-runs-defer", r#"define class Counter as pub let int value=0;end
          fnc update(borrow mut Counter counter):void{defer counter.value=99;for(let int index=0;index<2;index=index+1){if index==1;then return;fi}return;}
          pub fnc main():int{let mut Counter counter=new Counter();update(rt mut borrow counter);return counter.value==99?0:1;}"#, ""),
        ("foreach-return-releases-loan-before-defer", r#"define class Box as pub val Array<int> values;pub let int code=7;end
          fnc pick(borrow mut Box owner):int{defer owner.code=99;for(const item of owner.values){return owner.code;}return -1;}
          pub fnc main():int{val Array<int> values=new Array<int>();values.add(1);val Box owner=new Box(values);val selected:int=pick(rt mut borrow owner);return selected==7&&owner.code==99?0:1;}"#, ""),
        ("index-loop-branch-exits-and-string-result", r#"fnc choose_text(bool first):String{for(let int index=0;index<1;index=index+1){if(first){val String result="yes";return result;}else{val String result="no";return result;}fi}return "empty";}
          pub fnc main():int{return choose_text(true)=="yes"&&choose_text(false)=="no"?0:1;}"#, ""),
        ("index-loop-update-order-and-zero-iterations", r#"define class Counter as pub let int value=0;end
          pub fnc main():int{let mut Counter counter=new Counter();for(let int index=0;index<4;index=index+1){counter.value=counter.value+index;}
          for(let int index=4;index<4;index=index+1){counter.value=999;}return counter.value==6?0:1;}"#, ""),
        ("actual-routing-path-validation", include_str!("fixtures/native-class-string/routing-path-validation.ores"), ""),
        ("foreach-return-unwinds-scopes", r#"fnc first(bool fill):int{val Array<int> values=new Array<int>();if(fill){values.add(42);}else{}fi for(const item of values){return item;}return 0;}pub fnc main():int{return first(true)==42&&first(false)==0?0:1;}"#, ""),
        ("index-return-snapshot-before-deferred-field-write", r#"define class Counter as pub let int value=7;end
          fnc choose(borrow mut Counter counter):int{defer counter.value=99;for(let int index=0;index<3;index=index+1){if index==1;then val int selected=counter.value;return selected;fi}return -1;}
          pub fnc main():int{let mut Counter counter=new Counter();val selected:int=choose(rt mut borrow counter);return selected==7&&counter.value==99?0:1;}"#, ""),
        ("nested-read-loans-release-after-loop", r#"define class Counter as pub let int total=0;end
          pub routine main():int{val Array<int> values=new Array<int>();values.add(1);values.add(2);let mut Counter counter=new Counter();
          for(const first of values){for(const second of values){counter.total=counter.total+first+second;}}values.add(3);return counter.total==12&&values.size==3?0:1;}"#, ""),
        ("typed-router-copy-concat-iteration", include_str!("fixtures/native-class-string/typed-array-iteration.ores"), "foo\nid\n"),
        ("string-array-copy-empty-and-nul", r#"define class RequestPath as
          pub val Array<String> segments;
          pub copy():RequestPath{val Array<String> copied=new Array<String>();for(const segment of self.segments){copied.add(segment);}return new RequestPath(copied);}
          end pub routine main():int{val Array<String> a=new Array<String>();a.add("x\u0000y");a.add("é");val RequestPath p=new RequestPath(a);val RequestPath q=p.copy();
          val Array<String> empty=new Array<String>();val RequestPath e=new RequestPath(empty);val RequestPath f=e.copy();
          return q.segments.get(0)=="x\u0000y"&&q.segments.get(1)=="é"&&f.segments.size==0?0:1;}"#, ""),
        ("nested-iteration-scopes-and-branches", r#"define class Item as pub val int value;pub read():int{return self.value;}end
          define class Counter as pub let int value=0;end
          pub routine main():int{val Array<Item> a=new Array<Item>();a.add(new Item(2));a.add(new Item(3));
          val Array<int> b=new Array<int>();b.add(10);b.add(20);let mut Counter count=new Counter();
          for(const item of a){val Item view=a.get(0);for(const n of b){if(n==10){val int amount=view.read()+n;count.value=count.value+amount;}else{val int amount=item.value+n;count.value=count.value+amount;}fi}}
          b.add(30);return count.value==69&&b.size==3?0:1;}"#, ""),

        ("exact-user-program", exact, "hello world\nhiiii 2hiiii"),
        ("user-two-argument-println",
            r#"import class Foo from './main-1.ores';
               pub routine main():int {
                 stdio.println("hello world");
                 val Foo f = new Foo();
                 val string x = f.bar();
                 stdio.println("x", x);
                 return 0;
               }"#,
            "hello world\nxhiiii 2hiiii\n"),
        ("variadic-console-evaluation-order",
            r#"fnc mark(string s):string{stdio.print(s);return s;}
               pub routine main():int{
                 stdio.println(mark("a"), mark("b"), "c");
                 return 0;
               }"#,
            "ababc\n"),
        ("variadic-print-utf8-nul",
            r#"pub routine main():int{
                 stdio.print("A", "\u0000", "é");
                 return 0;
               }"#,
            "A\0é"),
        ("variadic-console-at-boundary-32",
            r#"pub fnc main():int{stdio.println("x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x", "x");return 0;}"#,
            "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n"),
        (
            "imported-owning-class-return",
            r#"import class OwnedBox from './owned-box.ores';
              fnc make_box():OwnedBox{val Array<int> values=new Array<int>();values.add(42);return new OwnedBox(values);}
              pub routine main():int{val OwnedBox value=make_box();val OwnedBox copied=value.copy();return value.values.get(0)==42&&copied.values.get(0)==42?0:1;}"#,
            "",
        ),
        (
            "owning-mutable-aggregate-call-result-and-move",
            r#"define class Shelf as pub let int tag=1;pub let mut Array<int> first=new Array<int>();pub let mut Array<int> second=new Array<int>();end
              fnc make_shelf():Shelf{let mut Shelf value=new Shelf();value.first.add(13);value.second.add(29);return value;}
              fnc edit(borrow mut Shelf value):void{value.first.set(0,14);return;}
              pub routine main():int{val Shelf value=true?make_shelf():make_shelf();let mut Shelf moved=rt take value;edit(rt mut borrow moved);return moved.tag+moved.first.get(0)+moved.second.get(0)==44?0:1;}"#,
            "",
        ),
        (
            "owning-string-array-constructor-conditional-return",
            r#"define class Names as pub val Array<String> values;end
              fnc make_names(bool first):Names{val Array<String> values=new Array<String>();values.add("native");return first?new Names(values):new Names(rt take values);}
              pub routine main():int{val Names a=make_names(true);val Names b=make_names(false);stdio.println(a.values.get(0));return b.values.get(0)=="native"?0:1;}"#,
            "native\n",
        ),
        (
            "owned-aggregate-return-and-forwarding",
            include_str!("fixtures/native-class-string/owned-aggregate-return.ores"),
            "/foo/:id\nid\n",
        ),
        (
            "imported-class-return-and-borrowed-method",
            r#"import class Foo from './main-1.ores';
              fnc create():Foo{return new Foo();}
              pub routine main():int{val Foo value=create();stdio.println(value.bar());return 0;}"#,
            "hiiii 2hiiii\n",
        ),
        (
            "lazy-record-return-forwarding",
            r#"define class Value as pub val int code;end
              fnc fresh():Value{return new Value(42);}
              fnc choose_record(bool use_new):Value{return use_new?new Value(42):fresh();}
              fnc same_owner(bool first):Value{val Value value=new Value(42);return first?value:value;}
              pub routine main():int{val Value a=choose_record(true);val Value b=choose_record(false);val Value c=same_owner(true);val Value d=same_owner(false);return a.code==42&&b.code==42&&c.code==42&&d.code==42?0:1;}"#,
            "",
        ),
        (
            "immutable-record-returns-after-callee-cleanup",
            include_str!("fixtures/native-class-string/record-return.ores"),
            "segment\nlocal\n",
        ),
        (
            "owned-path-pattern-borrowed-methods",
            include_str!("fixtures/native-class-string/owned-path-pattern.ores"),
            "/foo/:id\nfoo\n",
        ),
        (
            "nested-borrows-distinguish-array-regions",
            r#"define class Box as pub val Array<int> values; end
              fnc sum(borrow Box left,borrow Box right):int{return left.values.get(0)+right.values.get(0);}
              fnc nested(borrow Box parent):int {
                val Array<int> numbers=new Array<int>(); numbers.add(29);
                val Box child=new Box(numbers);
                return sum(rt borrow parent,rt borrow child);
              }
              pub routine main():int {
                val Array<int> numbers=new Array<int>(); numbers.add(13);
                val Box owner=new Box(numbers);
                return nested(rt borrow owner)==42?0:1;
              }"#,
            "",
        ),
        (
            "exclusive-class-array-borrow",
            r#"define class Box as pub val Array<int> values; end
              fnc append(borrow mut Box owner):void {owner.values.add(29);return;}
              pub routine main():int {
                val Array<int> numbers=new Array<int>(); numbers.add(13);
                val Box owner=new Box(rt take numbers);
                append(rt mut borrow owner);
                return owner.values.size==2 && owner.values.get(1)==29?0:1;
              }"#,
            "",
        ),
        (
            "borrowed-string-array-field",
            r#"define class Names as val Array<String> values;
                pub first():String{return self.values.get(0);}
              end
              pub routine main():int {
                val Array<String> names=new Array<String>();names.add("route");
                val Names owner=new Names(names);
                stdio.println(owner.first());return 0;
              }"#,
            "route\n",
        ),
        (
            "immutable-record-array-replacement",
            include_str!("fixtures/native-class-string/record-array.ores"),
            "foo\ntail\n",
        ),
        (
            "imported-record-array-borrowed-method",
            r#"import class Foo from './main-1.ores';
              pub routine main():int {
                val Array<Foo> values=new Array<Foo>();
                values.add(new Foo());
                val Foo first=values.get(0);
                stdio.println(first.bar());
                return 0;
              }"#,
            "hiiii 2hiiii\n",
        ),
        (
            "routing-path-segment-record",
            include_str!("fixtures/native-class-string/path-segment.ores"),
            "foo\n",
        ),
        (
            "typed-bool-and-string-record",
            r#"define class Header as pub val bool enabled; pub val String text; end
              pub routine main():int {
                val Header header=new Header(true,"Content-Type");
                stdio.println(header.text);
                return header.enabled && header.text == "Content-Type" ? 0 : 1;
              }"#,
            "Content-Type\n",
        ),
        (
            "string-field-literal-initializer",
            r#"define class Label as pub val String text="ready"; end
              pub routine main():int {val Label label=new Label();stdio.println(label.text);return 0;}"#,
            "ready\n",
        ),
        (
            "typed-string-array-path-segments",
            include_str!("fixtures/native-class-string/string-array.ores"),
            "foo\nbaz\n",
        ),
        (
            "string-array-retains-immutable-values",
            r#"pub routine main():int {
                let mut Array<string> values = new Array<string>();
                values.add("a\u0000b");
                values.add(values.get(0));
                values.set(0, "changed");
                return values.size == 2 && values.get(1) == "a"+"\u0000b" && values.get(0) == "changed" ? 0 : 1;
              }"#,
            "",
        ),
        (
            "nats-string-contains-literal-binary",
            r#"fnc valid_subject_token(String token):bool {
                  if token == "" then return false; fi
                  if token.contains_literal(" ") || token.contains_literal("\t") ||
                     token.contains_literal("\r") || token.contains_literal("\n") ||
                     token.contains_literal("\u0000") then return false; fi
                  return true;
                }
                pub routine main():int {
                  val String subject="orders.created.eu";
                  val String nul="a\u0000b";
                  val String utf="é\u0000stop";
                  return valid_subject_token("orders") &&
                    !valid_subject_token("") &&
                    !valid_subject_token("bad subject") &&
                    !valid_subject_token(nul) &&
                    subject.contains_literal("created") &&
                    subject.contains_literal(".") &&
                    subject.contains_literal("") &&
                    !subject.contains_literal("payments") &&
                    !subject.contains_literal("orders.created.eu.extra") &&
                    nul.contains_literal("\u0000") &&
                    utf.contains_literal("\u0000") &&
                    !subject.contains_literal("\u0000") ? 0 : 1;
                }"#,
            "",
        ),
        (
            "string-content-equality",
            r#"fnc same(string a,string b):bool{return a == b;}
              pub routine main():int {
                val string a="foo"; val string b="f"+"oo";
                stdio.println(same(a,b) ? "equal" : "WRONG");
                stdio.println(a != "bar" ? "different" : "WRONG");
                stdio.println("" == "" ? "empty" : "WRONG");
                return 0;
              }"#,
            "equal\ndifferent\nempty\n",
        ),
        (
            "string-equality-embedded-nul-and-unicode",
            r#"pub routine main():int {
                stdio.println("a\u0000b" == "a"+"\u0000b" ? "nul" : "WRONG");
                stdio.println("a\u0000b" != "a" ? "length" : "WRONG");
                stdio.println("\u00e9" == "é" ? "unicode" : "WRONG");
                return 0;
              }"#,
            "nul\nlength\nunicode\n",
        ),
        (
            "http-string-routing-primitives",
            r#"fnc route(string method,string path):int {
                if (method != "GET") { return 405; } fi
                return path == "/health" ? 200 : 404;
              }
              pub routine main():int {
                stdio.println(route("G"+"ET", "/"+"health") == 200 ? "health" : "WRONG");
                stdio.println(route("GET", "/missing") == 404 ? "missing" : "WRONG");
                stdio.println(route("POST", "/health") == 405 ? "method" : "WRONG");
                return 0;
              }"#,
            "health\nmissing\nmethod\n",
        ),
        (
            "template-quote-vectors",
            r#"pub fnc main():int{
                stdio.print("abc");stdio.print("|");
                stdio.print("'abc'");stdio.print("|");
                stdio.print('\"abc\"');stdio.print("|");
                stdio.print(`'\"abc\"'`);return 0;}"#,
            "abc|'abc'|\"abc\"|'\"abc\"'",
        ),
        (
            "template-two-parameter-interpolation",
            r#"fnc render(string bar,string star):string{return `${bar} is ${star}`;}
              pub fnc main():int{stdio.println(render("Mars","bright"));return 0;}"#,
            "Mars is bright\n",
        ),
        (
            "template-nested-expression-and-quotes",
            r#"pub fnc main():int{val string word="ok";stdio.print(`before:${`inside:${word}`}:after`);
              stdio.print("|");stdio.print(`${"}"+"{"}`);return 0;}"#,
            "before:inside:ok:after|}{",
        ),
        (
            "template-escapes",
            r#"pub fnc main():int{stdio.print(`line\n\t\${literal}\`\u00e9`);return 0;}"#,
            "line\n\t${literal}`é",
        ),
        (
            "template-left-to-right-once",
            r#"fnc first():string{stdio.print("1");return "A";}
              fnc second():string{stdio.print("2");return "B";}
              pub fnc main():int{stdio.print(`${first()}-${second()}`);return 0;}"#,
            "12A-B",
        ),
        (
            "source-effect-order",
            r#"fnc mark():string{stdio.print("method;");return "a";}
          pub fnc main():int{stdio.print("before;");val string a=mark();stdio.print("after;");
          val string b=mark();stdio.print(a+b);return 0;}"#,
            "before;method;after;method;aa",
        ),
        (
            "parameters-and-return",
            r#"fnc join(string a,string b):string{return a+b;}
          pub fnc main():int{val string a=join("left", "right");stdio.println(a);return 0;}"#,
            "leftright\n",
        ),
        (
            "lazy-conditional",
            r#"fnc fail():string{stdio.print("WRONG");return "wrong";}
          pub fnc main():int{val string x=true ? "yes" : fail();stdio.print(x);return 0;}"#,
            "yes",
        ),
        (
            "escaped-unicode",
            r#"pub fnc main():int{stdio.print("\u00e9\uD83D\uDE00\n\t\"\\");return 0;}"#,
            "é😀\n\t\"\\",
        ),
        (
            "empty-and-embedded-nul",
            r#"pub fnc main():int{val string x=""+"a\u0000b";stdio.print(x+"");return 0;}"#,
            "a\0b",
        ),
    ];
    // Java does not yet admit named value arguments. Keep that ABI regression
    // explicit and native-only rather than weakening the common-source gate.
    let named_source = positives.iter().find(|case| case.0 == "nested-borrows-distinguish-array-regions").unwrap().1.replace(
        "sum(rt borrow parent,rt borrow child)",
        "sum(right:rt borrow child,left:rt borrow parent)",
    );
    // The pinned Java parser still requires an initializer for final class fields.
    let native_only = [
        // The pinned Java reference predates declare-type source syntax.
        ("ordered-transparent-alias-and-extension", "declare type Count = int;define mtd double on Count():int{return self+self;}fnc touch():void{return;}pub fnc main():int{touch();let value:Count=21;value=value.double();return value==42?0:1;}", ""),
        // Java currently treats variant calls in field defaults as unproven
        // effects; native lowering recognizes literal compiler-owned variants.
        ("literal-sum-field-defaults", r#"define class Holder as pub val Option<int> missing=None;pub val Option<bool> available=Some(false);pub val Result<String,int> result=Err(0);end pub routine main():int{val Holder holder=new Holder();return holder.missing.is_none()&&holder.available.is_some()&&!holder.available.unwrap()&&holder.result.is_err()?0:1;}"#, ""),
        ("named-borrows-distinguish-array-regions", named_source.as_str(), ""),
        ("readonly-nested-child-retains-read-access", r#"define class Data as pub val Array<int> values;pub read():int{return self.values.get(0);}end define class Outer as pub final Data child;end
          pub fnc main():int{val Array<int> values=new Array<int>();values.add(42);val Data child=new Data(values);val Outer outer=new Outer(child);return outer.child.read()==42&&outer.child.values.size==1?0:1;}"#, ""),
    ];
    let mut comparisons = 0;
    let mut strict_comparisons = 0;
    let mut case_evidence = Vec::<String>::new();
    let mut guest_address_checks = false;
    let panics = [
        ("sum-none-unwrap-controlled-abort", "pub routine main():int{val Option<int> item=None;return item.unwrap();}", ""),
        ("sum-err-unwrap-controlled-abort", "pub routine main():int{val Result<int,String> item=Err(\"missing\");return item.unwrap();}", ""),
    ];
    for (case_index, (name, source, expected)) in positives.iter().chain(native_only.iter()).chain(panics.iter()).enumerate() {
        let expects_panic = case_index >= positives.len() + native_only.len();
        // Canonical ADT regressions execute as actual .atto guest files.
        let guest_name = if name.starts_with("adt-conditional-") {
            "main.atto"
        } else { "main.ores" };
        fs::write(scratch.0.join(guest_name), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), foo).unwrap();
        fs::write(scratch.0.join("sums.ores"), "define module sums as pub fnc choose(bool yes):Option<string>{return yes?Some(\"module\"):None;}pub fnc checked(bool yes):Result<bool,string>{return yes?Ok(false):Err(\"missing\");}end").unwrap();
        fs::write(scratch.0.join("owned-box.ores"), "define pub class OwnedBox as pub val Array<int> values;pub copy():OwnedBox{val Array<int> copied=new Array<int>();copied.add(self.values.get(0));return new OwnedBox(copied);}end").unwrap();
        let captures_fixture = include_str!("fixtures/native-class-string/nested-owned-capture.ores")
            .replace("pub routine main():int", "pub fnc verify_capture():int");
        fs::write(scratch.0.join("captures.ores"), format!("define module captures as\n{captures_fixture}\nend\n")).unwrap();
        let module_fixture = include_str!("fixtures/native-class-string/routing-path-validation.ores")
            .replace("pub routine main(): int", "pub fnc verify_paths(): int");
        fs::write(scratch.0.join("paths.ores"), format!("define module paths as\n{module_fixture}\nend\n")).unwrap();
        let path = scratch.0.join(guest_name);
        let generated = run(Command::new(&compiler).arg(&path));
        assert!(
            generated.status.success() && !generated.stdout.is_empty(),
            "{name}: {}",
            String::from_utf8_lossy(&generated.stderr)
        );
        // Scope slots inside repeated control flow must never allocate per
        // iteration. Verify the generated CFG before linking, even at O0.
        let mut block = "";
        for line in std::str::from_utf8(&generated.stdout).unwrap().lines() {
            if let Some((label, _)) = line.split_once(':') {
                if !line.starts_with(' ') { block = label; }
            }
            if [".arm.stack", "record.fields", "record.argument", "sum.fields"].iter().any(|name| line.contains(name)) && line.contains("alloca ") {
                assert_eq!(block, "entry", "{name}: scope slot allocated on a control-flow edge");
            }
        }
        let ir = scratch.0.join("program.ll");
        let binary = scratch.0.join("program");
        let raw_ir = std::str::from_utf8(&generated.stdout).unwrap();
        // Clang's -fsanitize flag alone does not mark imported LLVM IR
        // functions for ASan. Mark every definition, then verify that actual
        // guest-memory check calls are emitted (not just runtime linking).
        let checked_ir = if sanitize {
            raw_ir.lines().map(|line| {
                if line.starts_with("define ") {
                    format!("{} sanitize_address {{\n", line.strip_suffix('{').expect("function definition"))
                } else { format!("{line}\n") }
            }).collect::<String>()
        } else { raw_ir.to_owned() };
        fs::write(&ir, checked_ir).unwrap();
        if sanitize && *name == "nested-constructor-result-and-grandchild" {
            let checked = run(Command::new(&clang).args(["-O1", "-S", "-emit-llvm", "-fsanitize=address", "-x", "ir"])
                .arg(&ir).args(["-o", "-"]));
            assert!(checked.status.success(), "guest ASan: {}", String::from_utf8_lossy(&checked.stderr));
            let instrumented = String::from_utf8_lossy(&checked.stdout);
            assert!(["call void @__asan_report_load8", "call void @__asan_report_store8",
                "call void @__asan_report_load_n", "call void @__asan_report_store_n"].iter().any(|call| instrumented.contains(call)),
                "guest LLVM memory accesses were not instrumented");
            guest_address_checks = true;
        }
        if binary.exists() {
            fs::remove_file(&binary).unwrap();
        }
        let mut link = Command::new(&clang);
        link.args(["-O1", "-g"]);
        if sanitize {
            link.args([
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer",
            ]);
        }
        let linked = run(link
            .arg("-x")
            .arg("ir")
            .arg(&ir)
            .args(["-x", "c", "-std=c11"])
            .arg("-I")
            .arg(PathBuf::from(&runtime).parent().unwrap().parent().unwrap().parent().unwrap())
            .arg(&runtime)
            .arg(PathBuf::from(&runtime).parent().unwrap().join("NativeIntArrayRegion.c"))
            .arg(PathBuf::from(&runtime).parent().unwrap().join("NativeStringSplit.c"))
            .arg(PathBuf::from(&runtime).parent().unwrap().join("NativeRecordRegion.c"))
            .arg("-pthread")
            .arg("-o")
            .arg(&binary));
        assert!(
            linked.status.success() && binary.is_file(),
            "{name}: {}",
            String::from_utf8_lossy(&linked.stderr)
        );
        let strict_case = require_java && case_index < positives.len() && JAVA_LLVM_STRICT_NINE.contains(name);
        let mut native_stdout = Vec::<u8>::new();
        for _ in 0..5 {
            let native = run(&mut Command::new(&binary));
            if expects_panic {
                use std::os::unix::process::ExitStatusExt;
                assert_eq!(native.status.signal(), Some(6), "{name}: expected explicit SIGABRT: {}", String::from_utf8_lossy(&native.stderr));
                assert!(native.stdout.is_empty());
                assert!(native.stderr.is_empty(), "{name}: unexpected memory/runtime failure: {}", String::from_utf8_lossy(&native.stderr));
                continue;
            }
            assert_eq!(
                native.status.code(),
                Some(0),
                "{name}: {}",
                String::from_utf8_lossy(&native.stderr)
            );
            assert_eq!(native.stdout, expected.as_bytes(), "{name}: native output");
            if strict_case { native_stdout = native.stdout.clone(); }
            assert!(
                native.stderr.is_empty(),
                "{name}: sanitizer/runtime diagnostics: {}",
                String::from_utf8_lossy(&native.stderr)
            );
        }
        let execute_java = java.is_some() && (if require_java { strict_case } else { case_index < positives.len() || expects_panic });
        if execute_java {
            let launcher = java.as_ref().expect("Java launcher required");
            if expects_panic {
                let reference = run(Command::new(launcher).args(&java_args).arg(&path));
                assert!(!reference.status.success(), "Java {name}: invalid unwrap succeeded");
                comparisons += 1;
            } else {
                let java_runs = if require_java { 2 } else { 1 };
                let mut java_stdout = Vec::<u8>::new();
                for _ in 0..java_runs {
                    let reference = run(Command::new(launcher).args(&java_args).arg(&path));
                    assert_eq!(reference.status.code(), Some(0), "Java {name}: {}", String::from_utf8_lossy(&reference.stderr));
                    assert_eq!(reference.stdout, expected.as_bytes(), "Java {name}: output");
                    if require_java {
                        assert!(reference.stderr.is_empty(), "Java {name}: unexpected diagnostics: {}", String::from_utf8_lossy(&reference.stderr));
                    }
                    java_stdout = reference.stdout;
                }
                if strict_case {
                    assert_eq!(java_stdout, native_stdout, "Java/LLVM {name}: byte-differential output");
                    case_evidence.push(format!(
                        r#"{{"name":"{name}","native_stdout_hex":"{}","java_stdout_hex":"{}","native_exit":0,"java_exit":0,"native_runs":5,"java_runs":2}}"#,
                        hex_encode(&native_stdout), hex_encode(&java_stdout)
                    ));
                    strict_comparisons += 1;
                }
                comparisons += 1;
            }
        }
    }
    check_sum_boundary(&compiler, &clang, &runtime, &scratch.0, sanitize);
    fs::write(scratch.0.join("sum-rt.ores"), "fnc relay(Option<int> item):Option<int>{return item;}pub fnc main():int{return 0;}").unwrap();
    let rt = run(Command::new(&compiler).args(["--rt-check", "--rt-entry=relay/1"]).arg(scratch.0.join("sum-rt.ores")));
    assert_eq!(rt.status.code(), Some(1), "sum passthrough cannot be hard-RT certified");
    let report = String::from_utf8(rt.stdout).unwrap();
    assert!(report.contains("\"source_admitted\": false") && report.contains("RT009_SCHEDULING_UNPROVEN") && !report.contains("define "), "sum RT did not return a refusal report: {report}");
    fs::write(scratch.0.join("ordered-rt.ores"), "define class Info as pub val int code;end fnc audit():int{if false;then return 0;fi val Info snapshot=new Info(1);return snapshot.code;}pub fnc main():int{return 0;}").unwrap();
    let rt = run(Command::new(&compiler).args(["--rt-check", "--rt-entry=audit/0"]).arg(scratch.0.join("ordered-rt.ores")));
    assert_eq!(rt.status.code(), Some(1), "ordered record materialization cannot be hard-RT certified");
    let report = String::from_utf8(rt.stdout).unwrap();
    assert!(report.contains("\"source_admitted\": false") && !report.contains("define "), "ordered record RT lacks a refusal report: {report}");
    let mut standalone_runs = 0;
    if let Some(driver) = &aot_driver {
        #[cfg(unix)] {
            use std::os::unix::{fs::PermissionsExt, fs::symlink};
            let source = scratch.0.join("protected.ores");
            fs::write(&source, "pub fnc main():int{return 0;}").unwrap();
            let fake_compiler = scratch.0.join("compiler");
            let fake_clang = scratch.0.join("clang");
            let sentinel = b"#!/bin/sh\necho 'must not execute' >&2\nexit 99\n";
            for path in [&fake_compiler, &fake_clang] {
                fs::write(path, sentinel).unwrap();
                fs::set_permissions(path, fs::Permissions::from_mode(0o755)).unwrap();
            }
            let symbolic = scratch.0.join("compiler-symlink");
            symlink(&fake_compiler, &symbolic).unwrap();
            let hard = scratch.0.join("clang-hardlink");
            fs::hard_link(&fake_clang, &hard).unwrap();
            for destination in [&fake_compiler, &fake_clang, &symbolic, &hard] {
                let rejected = run(Command::new(driver).env("ORESLANG_LLVMC", &fake_compiler)
                    .env("CLANG", &fake_clang).arg(&source).arg(destination));
                assert_eq!(rejected.status.code(), Some(64), "AOT alias guard: {}", String::from_utf8_lossy(&rejected.stderr));
                assert!(String::from_utf8_lossy(&rejected.stderr).contains("must not alias"));
                assert_eq!(fs::read(destination).unwrap(), sentinel);
            }
        }
        let scalar_record = r#"define class Number as pub val int code;end
          fnc create():Number{return new Number(42);}
          pub routine main():int{val Number value=create();return value.code==42?0:1;}"#;
        let owning_int = r#"define class Box as pub val Array<int> values;end
          fnc create():Box{val Array<int> a=new Array<int>();a.add(42);return new Box(a);}
          pub routine main():int{val Box value=create();return value.values.get(0)==42?0:1;}"#;
        for (source, expected) in [
            (include_str!("fixtures/native-class-string/ordered-routing-locals.ores"), "enter;enter;initialize;finish;done;"),
            (include_str!("fixtures/native-class-string/routing-option-capture.ores"), ""),
            (include_str!("fixtures/native-class-string/nested-owned-capture.ores"), "id\n"),
            (include_str!("fixtures/native-class-string/routing-path-validation.ores"), ""),
            (include_str!("fixtures/native-class-string/typed-array-iteration.ores"), "foo\nid\n"),
            (scalar_record, ""),
            (owning_int, ""),
            (include_str!("fixtures/native-class-string/record-return.ores"), "segment\nlocal\n"),
            (include_str!("fixtures/native-class-string/owned-aggregate-return.ores"), "/foo/:id\nid\n"),
        ] {
            let path = scratch.0.join("standalone.ores");
            let binary = scratch.0.join("standalone");
            fs::write(&path, source).unwrap();
            let built = run(Command::new(driver)
                .env("ORESLANG_LLVMC", fs::canonicalize(&compiler).unwrap())
                .env("CLANG", &clang).arg(&path).arg(&binary));
            assert!(built.status.success(), "AOT driver: {}", String::from_utf8_lossy(&built.stderr));
            fs::remove_file(&path).unwrap();
            let executed = run(Command::new(&binary).env_clear().env("PATH", ""));
            assert_eq!(executed.status.code(), Some(0), "source-free AOT: {}", String::from_utf8_lossy(&executed.stderr));
            assert_eq!(executed.stdout, expected.as_bytes());
            assert!(executed.stderr.is_empty());
            standalone_runs += 1;
        }
    }
    let negatives = [
        ("ordered-let-array-without-mut-cannot-mutate", "fnc touch():void{return;}pub fnc main():int{touch();let Array<int> values=new Array<int>();values.add(1);return 0;}".to_owned(), foo.to_owned()),

        ("ordered-array-readonly-cannot-mutate", "fnc touch():void{return;}pub fnc main():int{touch();const Array<int> values=new Array<int>();values.add(1);return 0;}".to_owned(), foo.to_owned()),
        ("ordered-array-constructor-type-mismatch", "fnc touch():void{return;}pub fnc main():int{touch();val Array<int> values=new Array<String>();return 0;}".to_owned(), foo.to_owned()),
        ("ordered-array-element-type-mismatch", "fnc touch():void{return;}pub fnc main():int{touch();val Array<String> values=new Array<String>();values.add(1);return 0;}".to_owned(), foo.to_owned()),
        ("checked-copy-option-reject-string", "pub routine main():int{val Array<String> values=new Array<String>();values.add(\"x\");val Option<int> forged=values.get_copy(0);return 0;}".to_owned(), foo.to_owned()),
        ("checked-copy-option-reject-bool-index", "pub routine main():int{val Array<int> values=new Array<int>();values.add(42);val Option<int> forged=values.get_copy(true);return 0;}".to_owned(), foo.to_owned()),
        ("checked-copy-option-reject-arity", "pub routine main():int{val Array<int> values=new Array<int>();val Option<int> forged=values.get_copy();return 0;}".to_owned(), foo.to_owned()),
        ("ordered-array-loop-cannot-retain-borrow", "fnc touch():void{return;}pub fnc main():int{touch();for(let int i=0;i<2;i=i+1){val Array<int> values=new Array<int>();values.add(i);}return values.get(0);}".to_owned(), foo.to_owned()),

        ("legacy-mut-borrow-spelling-stays-rejected", "pub fnc main():int{val int owner=1;return rt borrow mut owner;}".to_owned(), foo.to_owned()),
        ("ordered-const-cannot-lend-exclusively", "fnc touch():void{return;}fnc observe(borrow mut int value):int{return 42;}pub fnc main():int{touch();const int value=42;return observe(rt mut borrow value);}".to_owned(), foo.to_owned()),
        ("ordered-array-cannot-escape-branch", "fnc touch():void{return;}pub fnc main():int{touch();if true;then val Array<int> values=new Array<int>();values.add(1);fi return values.size;}".to_owned(), foo.to_owned()),
        ("ordered-async-frame-not-on-stack", "define actor Worker as {let total:int=0;pub fnc on_message(int n):int{return n;}}pub fnc main():int{const worker:ActorRef<Worker> =spawn Worker();await worker.get_ready();let int value=1;stop worker;return value;}".to_owned(), foo.to_owned()),
        ("ordered-borrow-mut-parameter-not-reassignable", "fnc bad(borrow mut int value):int{value=2;return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("ordered-value-parameter-not-reassignable", "fnc bad(int value):int{value=2;return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("ordered-local-not-visible-before-initializer", "fnc touch():void{return;}pub fnc main():int{touch();let int value=value+1;return value;}".to_owned(), foo.to_owned()),
        ("ordered-local-not-visible-before-declaration", "fnc use(int value):void{return;}pub fnc main():int{use(value);let int value=1;return value;}".to_owned(), foo.to_owned()),
        ("ordered-const-cannot-assign", "fnc touch():void{return;}pub fnc main():int{touch();const int value=1;value=2;return value;}".to_owned(), foo.to_owned()),
        ("ordered-val-cannot-assign", "fnc touch():void{return;}pub fnc main():int{touch();val int value=1;value=2;return value;}".to_owned(), foo.to_owned()),
        ("ordered-shared-borrow-cannot-assign", "fnc bad(borrow int value):int{value=2;return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("ordered-bool-assignment-rejects-int", "fnc touch():void{return;}pub fnc main():int{touch();let bool value=false;value=1;return 0;}".to_owned(), foo.to_owned()),
        ("ordered-sum-assignment-rejects-payload", "fnc touch():void{return;}pub fnc main():int{touch();let Option<int> value=None;value=Some(true);return 0;}".to_owned(), foo.to_owned()),
        ("ordered-sum-assignment-rejects-integer", "fnc touch():void{return;}pub fnc main():int{touch();let Option<int> value=None;value=1;return 0;}".to_owned(), foo.to_owned()),
        ("ordered-branch-local-cannot-escape", "fnc touch():void{return;}pub fnc main():int{touch();if true;then let int value=1;fi return value;}".to_owned(), foo.to_owned()),
        ("ordered-owner-cannot-be-replaced", "define class Box as pub val Array<int> values;end pub fnc main():int{val Array<int> values=new Array<int>();val Box owner=new Box(values);owner=1;return 0;}".to_owned(), foo.to_owned()),
        ("ordered-loop-cannot-consume-outer-owner", "fnc touch():void{return;}fnc use(int value):void{return;}pub fnc main():int{touch();let int value=1;for(let int index=0;index<2;index=index+1){use(rt take value);}return 0;}".to_owned(), foo.to_owned()),
        ("ordered-local-shadows-owner", "fnc touch():void{return;}pub fnc main():int{let int value=1;touch();let int value=2;return value;}".to_owned(), foo.to_owned()),
        ("sum-contract-not-int-parameter", "pub define contract Api as fnc inspect(int value)=>int;end define module Server conforms Api as pub fnc inspect(Option<int> value):int{return 0;}pub fnc main():int{return 0;}end".to_owned(), foo.to_owned()),
        ("sum-contract-not-int-result", "pub define contract Api as fnc inspect(int value)=>int;end define module Server conforms Api as pub fnc inspect(int value):Option<int>{return Some(value);}pub fnc main():int{return 0;}end".to_owned(), foo.to_owned()),
        ("sum-cannot-reorder-legacy-return-guards", "fnc bad():Option<int>{if(true){return Some(1);}fi if true;then return Some(2);fi return None;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("sum-cannot-forge-from-int", "pub fnc main():int{val Option<int> item=1;return 0;}".to_owned(), foo.to_owned()),
        ("adt-conditional-option-scalar-false",
            include_str!("fixtures/adt-conditional-v1/option-scalar-false.atto").to_owned(),
            foo.to_owned()),
        ("adt-conditional-option-scalar-true",
            include_str!("fixtures/adt-conditional-v1/option-scalar-true.atto").to_owned(),
            foo.to_owned()),
        ("adt-conditional-result-scalar-false",
            include_str!("fixtures/adt-conditional-v1/result-scalar-false.atto").to_owned(),
            foo.to_owned()),
        ("sum-cannot-return-as-int", "pub fnc main():int{val Option<int> item=Some(1);return item;}".to_owned(), foo.to_owned()),
        ("sum-cannot-use-as-bool", "pub fnc main():int{val Option<bool> item=Some(true);return item?0:1;}".to_owned(), foo.to_owned()),
        ("sum-cannot-cross-payload-types", "fnc read(Option<int> item):int{return item.unwrap();}pub fnc main():int{val Option<bool> item=Some(true);return read(item);}".to_owned(), foo.to_owned()),
        ("sum-cannot-cross-error-types", "fnc read(Result<int,String> item):int{return item.unwrap();}pub fnc main():int{val Result<int,bool> item=Ok(1);return read(item);}".to_owned(), foo.to_owned()),
        ("sum-wrong-variant", "pub fnc main():int{val Option<int> item=Ok(1);return 0;}".to_owned(), foo.to_owned()),
        ("sum-wrong-payload", "pub fnc main():int{val Option<int> item=Some(true);return 0;}".to_owned(), foo.to_owned()),
        ("sum-wrong-error-payload", "pub fnc main():int{val Result<int,String> item=Err(1);return 0;}".to_owned(), foo.to_owned()),
        ("sum-wrong-fallback", "pub fnc main():int{val Option<int> item=None;return item.unwrap_or(true);}".to_owned(), foo.to_owned()),
        ("sum-wrong-method", "pub fnc main():int{val Option<int> item=None;return item.is_err()?0:1;}".to_owned(), foo.to_owned()),
        ("sum-wrong-method-arity", "pub fnc main():int{val Option<int> item=None;return item.is_none(1)?0:1;}".to_owned(), foo.to_owned()),
        ("sum-wrong-constructor-arity", "pub fnc main():int{val Option<int> item=Some(1,2);return 0;}".to_owned(), foo.to_owned()),
        ("sum-no-inferred-none", "pub fnc main():int{val item=None;return 0;}".to_owned(), foo.to_owned()),
        ("sum-no-nominal-shadow", "define class Option as val int value;end pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("sum-owned-class-payload-boundary", "define class Box as val Array<int> values;end fnc bad():Option<Box>{return None;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("sum-nested-payload-boundary", "fnc bad():Option<Option<int>>{return None;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("sum-not-os-status", "pub fnc main():Option<int>{return Some(0);}".to_owned(), foo.to_owned()),
        ("sum-constructor-cannot-reinterpret-scalar", "define class Box as val Option<int> item;constructor(int item){self.item=item;}end pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("nested-explicit-constructor-cannot-reinterpret-scalar", r#"define class Child as pub val int value;end define class Outer as val Child child;constructor(int child){self.child=child;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-depth-forward-order", format!("{}pub fnc main():int{{return 0;}}", (0..10).map(|i| if i==9 { "define class Level9 as val int value;end".to_owned() } else { format!("define class Level{i} as val Level{} child;end",i+1) }).collect::<String>()), foo.to_owned()),
        ("nested-depth-reverse-order", format!("{}pub fnc main():int{{return 0;}}", (0..10).rev().map(|i| if i==9 { "define class Level9 as val int value;end".to_owned() } else { format!("define class Level{i} as val Level{} child;end",i+1) }).collect::<String>()), foo.to_owned()),
        ("nested-expanded-layout-bound", format!("define class Child as {}end define class Outer as val Child child;val int extra;end pub fnc main():int{{return 0;}}", (0..16).map(|i| format!("val int field_{i};")).collect::<String>()), foo.to_owned()),
        ("final-primitive-projection-cannot-borrow-mut", r#"define class Counter as pub val int value;end fnc observe(borrow mut int value):int{return value;}pub fnc main():int{val Counter counter=new Counter(1);return observe(rt mut borrow counter.value);}"#.to_owned(), foo.to_owned()),
        ("readonly-child-edge-cannot-borrow-mut", r#"define class Child as pub val Array<int> values;end define class Outer as pub final Child child;end fnc edit(borrow mut Child child):void{child.values.add(1);return;}pub fnc main():int{val Array<int> values=new Array<int>();val Child child=new Child(values);val Outer outer=new Outer(child);edit(rt mut borrow outer.child);return 0;}"#.to_owned(), foo.to_owned()),
        ("readonly-child-edge-cannot-mutate-array", r#"define class Child as pub val Array<int> values;end define class Outer as pub final Child child;end pub fnc main():int{val Array<int> values=new Array<int>();val Child child=new Child(values);val Outer outer=new Outer(child);outer.child.values.add(1);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-duplicate-child-owner", r#"define class Child as pub val Array<int> values;end define class Pair as val Child first;val Child second;end pub fnc main():int{val Array<int> a=new Array<int>();val Child child=new Child(a);val Pair pair=new Pair(child,child);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-child-use-after-move", r#"define class Child as pub val Array<int> values;end define class Outer as val Child child;end pub fnc main():int{val Array<int> a=new Array<int>();val Child child=new Child(a);val Outer outer=new Outer(child);return child.values.size;}"#.to_owned(), foo.to_owned()),
        ("nested-borrowed-child-consumption", r#"define class Child as pub val Array<int> values;end define class Outer as val Child child;end fnc bad(borrow Child child):int{val Outer outer=new Outer(child);return 0;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-wrong-nominal-child", r#"define class A as pub val int value;end define class B as pub val int value;end define class Outer as val A child;end pub fnc main():int{val B child=new B(1);val Outer outer=new Outer(child);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-integer-forged-child", r#"define class Child as pub val int value;end define class Outer as val Child child;end pub fnc main():int{val Outer outer=new Outer(1);return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-parent-private-edge", r#"define class Child as pub val int value;end define class Outer as val Child child;end pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return outer.child.value;}"#.to_owned(), foo.to_owned()),
        ("nested-child-private-edge", r#"define class Child as val int value;end define class Outer as pub val Child child;pub leak():int{return self.child.value;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-private-child-loan", r#"define class Child as pub val int value;end define class Outer as val Child child;end fnc read(borrow Child child):int{return child.value;}pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return read(rt borrow outer.child);}"#.to_owned(), foo.to_owned()),
        ("nested-shared-to-exclusive-escalation", r#"define class Child as pub val Array<int> values;end define class Outer as pub val Child child;end fnc edit(borrow mut Child child):void{child.values.add(1);return;}fnc bad(borrow Outer outer):void{edit(rt mut borrow outer.child);return;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-projection-alias-conflict", r#"define class Child as pub val int value;end define class Outer as pub val Child child;end fnc pair(borrow mut Child a,borrow Child b):int{return b.value;}pub fnc main():int{val Child child=new Child(1);val Outer outer=new Outer(child);return pair(rt mut borrow outer.child,rt borrow outer.child);}"#.to_owned(), foo.to_owned()),
        ("nested-owning-field-cannot-escape-by-value", r#"define class Child as pub val Array<int> values;end define class Outer as pub val Child child;pub leak():Child{return self.child;}end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-cycle", r#"define class Node as val Node child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-indirect-cycle", r#"define class A as val B child;end define class B as val A child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-mutable-child-slot", r#"define class Child as pub let int value=1;end define class Outer as val Child child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-fieldless-identity", r#"define class Empty as end define class Outer as val Empty child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),
        ("nested-unknown-child-type", r#"define class Outer as val Missing child;end pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),

        ("index-loop-shadows-live-local", r#"pub fnc main():int{val index:int=0;for(let int index=0;index<1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-counter-escape", r#"pub fnc main():int{for(let int index=0;index<1;index=index+1){}return index;}"#.to_owned(), foo.to_owned()),
        ("index-initializer-wrong-type", r#"pub fnc main():int{for(let int index="bad";index<1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-condition-wrong-type", r#"pub fnc main():int{for(let int index=0;index+1;index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-update-wrong-type", r#"pub fnc main():int{for(let int index=0;index<1;index=true){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-updates-other-counter", r#"pub fnc main():int{val other:int=0;for(let int index=0;index<1;other=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-backedge-consumes-outer", r#"fnc consume(int n):void{return;}pub fnc main():int{val external_value:int=42;for(let int index=0;index<2;index=index+1){consume(rt take external_value);}return 0;}"#.to_owned(), foo.to_owned()),
        ("index-condition-consumes-counter", r#"fnc yes(int n):bool{return true;}pub fnc main():int{for(let int index=0;yes(rt take index);index=index+1){}return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-return-wrong-type", r#"pub fnc main():int{for(let int index=0;index<1;index=index+1){return true;}return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-return-ownership-transfer", r#"pub fnc main():int{val external_value:int=42;for(let int index=0;index<1;index=index+1){return rt take external_value;}return 0;}"#.to_owned(), foo.to_owned()),
        ("late-defer-after-conditional-return", r#"fnc cleanup():void{return;}pub fnc main():int{for(let int index=0;index<1;index=index+1){return 0;}defer cleanup();return 0;}"#.to_owned(), foo.to_owned()),
        ("structured-owning-class-return", r#"define class Box as pub val Array<int> values;end fnc bad():Box{val Array<int> values=new Array<int>();val Box box=new Box(values);for(let int index=0;index<1;index=index+1){return box;}return box;}pub fnc main():int{return 0;}"#.to_owned(), foo.to_owned()),

        ("iteration-backedge-consumes-scalar-owner", r#"fnc consume(int value):void{return;}pub fnc main():int{val Array<int> values=new Array<int>();values.add(1);val outer:int=42;for(const item of values){consume(rt take outer);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-record-alias-without-borrow", r#"define class Item as pub val int value;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const item of a){val Item other=item;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutates-source", r#"pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);for(const n of a){a.add(n);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutates-source-field", r#"define class Box as pub val Array<int> a;end pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Box b=new Box(a);for(const n of b.a){b.a.set(0,n);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-exclusive-owner-helper", r#"define class Box as pub val Array<int> a;end fnc edit(borrow mut Box b):void{b.a.add(1);return;}pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Box b=new Box(a);for(const n of b.a){edit(rt mut borrow b);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-consumes-outer-owner", r#"define class Box as pub val Array<int> a;end fnc consume(Box b):void{return;}pub fnc main():int{val Array<int> a=new Array<int>();a.add(1);val Array<int> b=new Array<int>();val Box other=new Box(b);for(const n of a){consume(rt take other);}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-scope-escape", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){}return n;}"#.to_owned(), foo.to_owned()),
        ("iteration-local-scope-escape", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){val int x=n;}return x;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-shadow", r#"pub fnc main():int{val Array<int> a=new Array<int>();val int n=0;for(const n of a){}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-nested-shadow", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){for(const n of a){}}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-mutable-record", r#"define class Item as pub let int value=1;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const n of a){stdio.println(n.value);}return 0;}"#.to_owned(), foo.to_owned()),
        ("scoped-mutable-record-snapshot", r#"define class Item as pub let int value=1;end pub fnc main():int{val Item i=new Item();if(true){val Item j=i;}else{}fi return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-element-write", r#"define class Item as pub val int value;end pub fnc main():int{val Array<Item> a=new Array<Item>();for(const n of a){n.value=2;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-break-without-cleanup", r#"pub fnc main():int{val Array<int> a=new Array<int>();for(const n of a){break;}return 0;}"#.to_owned(), foo.to_owned()),
        ("iteration-non-array-source", r#"pub fnc main():int{val int a=1;for(const n of a){}return 0;}"#.to_owned(), foo.to_owned()),

        ("owning-return-duplicate-array", "define class Pair as val Array<int> a;val Array<int> b;end fnc bad():Pair{val Array<int> values=new Array<int>();return new Pair(values,values);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-return-borrowed-field", "define class Box as pub val Array<int> values;end fnc bad(borrow Box value):Box{return new Box(value.values);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-return-deferred-source-reuse", "define class Box as pub val Array<int> values;end fnc observe_owner(borrow Box value):void{return;}fnc bad():Box{val Array<int> a=new Array<int>();val Box value=new Box(a);defer observe_owner(rt borrow value);return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("owning-result-use-after-class-move", "define class Box as pub val Array<int> values;end fnc make():Box{val Array<int> a=new Array<int>();return new Box(a);}pub fnc main():int{val Box a=make();let mut Box b=rt take a;return a.values.size;}".to_owned(), foo.to_owned()),
        ("owning-result-by-value-call", "define class Box as pub val Array<int> values;end fnc make():Box{val Array<int> a=new Array<int>();return new Box(a);}fnc observe_owner(Box value):int{return value.values.size;}pub fnc main():int{val Box b=make();return observe_owner(b);}".to_owned(), foo.to_owned()),
        ("record-return-deferred-owner-reuse", "define class Item as pub val int code;end fnc observe(borrow Item value):void{return;}fnc bad(bool first):Item{val Item value=new Item(42);defer observe(rt borrow value);return first?value:value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-integer-forgery", "define class Item as pub val int code;end fnc bad():Item{return 42;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-wrong-nominal", "define class A as pub val int code;end define class B as pub val int code;end fnc bad():A{return new B(1);}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-mutable-identity", "define class Item as pub let int code=42;end fnc bad():Item{return new Item();}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-borrowed-array-owner", "define class Box as val Array<int> values;end fnc bad(borrow Box value):Box{return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-incoming-borrow", "define class Item as pub val int code;end fnc bad(borrow Item value):Item{return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-moved-local", "define class Item as pub val int code;end fnc bad():Item{let mut Item value=new Item(1);let mut Item moved=rt take value;return value;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("record-return-not-os-status", "define class Item as pub val int code;end pub fnc main():Item{return new Item(42);}".to_owned(), foo.to_owned()),
        ("record-return-handle-as-int", "define class Item as pub val int code;end fnc make():Item{return new Item(42);}pub fnc main():int{return make();}".to_owned(), foo.to_owned()),
        ("record-return-unknown-type", "fnc bad():Missing{return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-use-after-move", "define class Box as val Array<int> values; end pub fnc main():int{val Array<int> a=new Array<int>();val Box b=new Box(a);return a.size;}".to_owned(), foo.to_owned()),
        ("constructor-array-duplicate-owner", "define class Pair as val Array<int> left;val Array<int> right;end pub fnc main():int{val Array<int> a=new Array<int>();val Pair p=new Pair(a,a);return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-element-mismatch", "define class Box as val Array<String> values;end pub fnc main():int{val Array<int> a=new Array<int>();val Box b=new Box(a);return 0;}".to_owned(), foo.to_owned()),
        ("shared-class-array-mutation", "define class Box as pub val Array<int> values;end fnc bad(borrow Box owner):void{owner.values.add(1);return;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("constructor-array-borrowed-field", "define class Box as pub val Array<int> values;end fnc bad(borrow Box owner):int{val Box other=new Box(owner.values);return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("class-array-by-value-escape", "define class Box as pub val Array<int> values;end fnc bad(Box owner):int{return owner.values.size;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("mutable-record-array", "define class Item as pub let int value=1; end pub fnc main():int{val Array<Item> a=new Array<Item>();return 0;}".to_owned(), foo.to_owned()),
        ("record-array-integer-forgery", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(1);return 0;}".to_owned(), foo.to_owned()),
        ("record-array-wrong-nominal-type", "define class A as pub val int value; end define class B as pub val int value; end pub fnc main():int{val Array<A> a=new Array<A>();a.add(new B(1));return 0;}".to_owned(), foo.to_owned()),
        ("record-array-handle-return", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));return a.get(0);}".to_owned(), foo.to_owned()),
        ("record-view-move", "define class Item as pub val int value; end pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);let mut Item moved=rt take item;return 0;}".to_owned(), foo.to_owned()),
        ("record-view-mutable-loan", "define class Item as pub val int value; end fnc edit(borrow mut Item item):int{return item.value;} pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);return edit(rt mut borrow item);}".to_owned(), foo.to_owned()),
        ("record-view-projected-mutable-loan", "define class Item as pub val int value; end fnc edit(borrow mut int value):int{return value;} pub fnc main():int{val Array<Item> a=new Array<Item>();a.add(new Item(1));val Item item=a.get(0);return edit(rt mut borrow item.value);}".to_owned(), foo.to_owned()),
        ("record-runtime-symbol-collision", "pub fnc ores_record_region_begin():int{return 0;}pub fnc main():int{return 0;}".to_owned(), foo.to_owned()),
        ("string-record-integer-argument", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment(1);return 0;}".to_owned(), foo.to_owned()),
        ("integer-record-string-argument", "define class Segment as pub val int kind; end pub fnc main():int{val Segment s=new Segment(\"bad\");return 0;}".to_owned(), foo.to_owned()),
        ("string-record-missing-argument", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment();return 0;}".to_owned(), foo.to_owned()),
        ("string-record-private-field", "define class Segment as val String value; end pub fnc main():int{val Segment s=new Segment(\"secret\");return s.value==\"secret\"?0:1;}".to_owned(), foo.to_owned()),
        ("string-record-final-field-write", "define class Segment as pub val String value; end pub fnc main():int{let mut Segment s=new Segment(\"first\");s.value=\"other\";return 0;}".to_owned(), foo.to_owned()),
        ("string-record-untyped-handle-return", "define class Segment as pub val String value; end pub fnc main():int{val Segment s=new Segment(\"first\");return s.value;}".to_owned(), foo.to_owned()),
        ("atto-nats-split-int-receiver", "pub fnc main():int{val int subject=42;val parts=subject.split_literal(\".\");return 0;}".to_owned(), foo.to_owned()),
        ("atto-nats-split-int-delimiter", "pub fnc main():int{val String subject=\"a.b\";val parts=subject.split_literal(42);return 0;}".to_owned(), foo.to_owned()),
        ("atto-nats-split-wrong-arity", "pub fnc main():int{val String subject=\"a.b\";val parts=subject.split_literal(\".\",\"-\");return 0;}".to_owned(), foo.to_owned()),
        ("atto-nats-split-named-delimiter", "pub fnc main():int{val String subject=\"a.b\";val parts=subject.split_literal(delimiter:\".\");return 0;}".to_owned(), foo.to_owned()),
        ("atto-nats-split-owner-as-int", "pub fnc main():int{val String subject=\"a.b\";val parts=subject.split_literal(\".\");val int token=parts;return token;}".to_owned(), foo.to_owned()),
        ("nats-native-string-abi-spoof", "pub fnc ores_string_split_literal():int{return 42;}pub fnc main():int{return ores_string_split_literal();}".to_owned(), foo.to_owned()),
        ("nats-native-string-containment-abi-spoof", "pub fnc ores_string_contains_literal():int{return 42;}pub fnc main():int{return ores_string_contains_literal();}".to_owned(), foo.to_owned()),
        ("nats-string-contains-int-receiver", "pub routine main():int{val n: int = 42;return n.contains_literal(\"4\")?0:1;}".to_owned(), foo.to_owned()),
        ("nats-string-contains-int-needle", "pub routine main():int{val String s=\"abc\";return s.contains_literal(42)?0:1;}".to_owned(), foo.to_owned()),
        ("nats-string-contains-wrong-arity", "pub routine main():int{val String s=\"abc\";return s.contains_literal(\"a\",\"b\")?0:1;}".to_owned(), foo.to_owned()),
        ("nats-string-contains-named-needle-rejected", "pub routine main():int{val String s=\"abc\";return s.contains_literal(needle:\"a\")?0:1;}".to_owned(), foo.to_owned()),
        ("string-array-integer-add", "pub fnc main():int{val Array<String> a=new Array<String>();a.add(42);return 0;}".to_owned(), foo.to_owned()),
        ("integer-array-string-add", "pub fnc main():int{val Array<int> a=new Array<int>();a.add(\"bad\");return 0;}".to_owned(), foo.to_owned()),
        ("string-array-string-index", "pub fnc main():int{val Array<String> a=new Array<String>();return a.get(\"bad\")==\"x\"?0:1;}".to_owned(), foo.to_owned()),
        ("string-array-integer-set", "pub fnc main():int{val Array<String> a=new Array<String>();a.set(0,42);return 0;}".to_owned(), foo.to_owned()),
        ("string-array-handle-as-int", "pub fnc main():int{val Array<String> a=new Array<String>();return a.get(0);}".to_owned(), foo.to_owned()),
        ("string-array-wrong-constructor", "pub fnc main():int{val Array<String> a=new Array<int>();return 0;}".to_owned(), foo.to_owned()),
        ("string-array-copy-as-int", "pub fnc main():int{val Array<String> a=new Array<String>();val n:int=a;return n;}".to_owned(), foo.to_owned()),
        ("string-array-take-into-int-field", "define class Box as pub mut Array<int> values=new Array<int>(); end pub fnc main():int{let mut Box b=new Box();val Array<String> a=new Array<String>();b.values=rt take a;return 0;}".to_owned(), foo.to_owned()),
        ("unsupported-object-array", "pub fnc main():int{val Array<Foo> a=new Array<Foo>();return 0;}".to_owned(), foo.to_owned()),
        (
            "string-integer-equality",
            r#"pub fnc main():int{return "a" == 1 ? 0 : 1;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "string-ordering",
            r#"pub fnc main():int{return "a" < "b" ? 0 : 1;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "template-non-string-interpolation",
            r#"pub fnc main():int{val string x=`${5}`;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "template-empty-interpolation",
            r#"pub fnc main():int{val string x=`${   }`;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "template-invalid-escape",
            r#"pub fnc main():int{val string x=`\q`;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "template-unclosed-interpolation",
            r#"pub fnc main():int{val string x=`${"x"`;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "private-method",
            exact.replace("f.bar()", "f.foo()"),
            foo.to_owned(),
        ),
        (
            "private-class",
            exact.to_owned(),
            foo.replace("define pub class", "define class"),
        ),
        (
            "unknown-class",
            exact.replace("import class Foo", "import class Missing"),
            foo.to_owned(),
        ),
        (
            "wrong-local-type",
            r#"pub fnc main():int{val x:int="bad";return x;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "wrong-return-type",
            r#"pub fnc main():int{return "bad";}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "wrong-console-type",
            "pub fnc main():int{stdio.print(42);return 0;}".to_owned(),
            foo.to_owned(),
        ),
        (
            "bad-concat",
            r#"pub fnc main():int{val string x="bad"+true;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "variadic-console-type-mismatch",
            r#"pub fnc main():int{stdio.println("a", 2);return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "variadic-console-too-many-args",
            format!("pub fnc main():int{{stdio.println({});return 0;}}",
                    vec!["\"x\""; 33].join(",")),
            foo.to_owned(),
        ),
        (
            "forward-local-read",
            r#"pub fnc main():int{stdio.print(x);val string x="bad";return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "unpaired-surrogate",
            r#"pub fnc main():int{stdio.print("\uD800");return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "runtime-symbol-collision",
            "fnc ores_string_literal(int x,int y):int{return 0;}pub fnc main():int{return 0;}"
                .to_owned(),
            foo.to_owned(),
        ),
        (
            "actor-string-region-boundary",
            r#"define actor Worker as {let total:int=0;pub fnc on_message(int n):int{return n;}}
                pub fnc main():int{stdio.print("unsafe");const worker:ActorRef<Worker> =spawn Worker();worker.send(1);stop worker;return 0;}"#.to_owned(),
            foo.to_owned(),
        ),
        (
            "print-is-void",
            r#"pub fnc main():int{return stdio.print("not an int");}"#.to_owned(),
            foo.to_owned(),
        ),
    ];
    for (name, source, dependency) in &negatives {
        let guest_name = if name.starts_with("adt-conditional-") {
            "main.atto"
        } else { "main.ores" };
        fs::write(scratch.0.join(guest_name), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), dependency).unwrap();
        let rejected = run(Command::new(&compiler).arg(scratch.0.join(guest_name)));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{name}: must fail before codegen"
        );
        assert!(rejected.stdout.is_empty(), "{name}: emitted partial IR");
        let expected_boundary = match *name {
            "legacy-mut-borrow-spelling-stays-rejected" => Some("legacy 'rt borrow mut value' is not accepted"),
            "checked-copy-option-reject-string" |
            "checked-copy-option-reject-bool-index" |
            "checked-copy-option-reject-arity" => Some("native Array.get_copy requires Array<int> and a checked int index"),
            "ordered-owning-array-not-hoisted" => Some("ordered owning local requires scoped region/drop lowering"),
            "ordered-async-frame-not-on-stack" => Some("continuation-aware statement lowering"),
            "sum-contract-not-int-parameter" => Some("contract ownership mode mismatch"),
            "adt-conditional-option-scalar-false" |
            "adt-conditional-option-scalar-true" |
            "adt-conditional-result-scalar-false" => Some(
                "native conditional sum branches must resolve to the same declared sum type"),
            "sum-contract-not-int-result" => Some("contract requires int return"),
            "owning-return-duplicate-array" => Some("live unique array"),
            "owning-return-borrowed-field" => Some("named lexical array"),
            "owning-return-deferred-source-reuse" => Some("borrow of moved class"),
            "owning-result-use-after-class-move" => Some("use-after-move"),
            "owning-result-by-value-call" => Some("by-value class parameter"),
            "record-return-integer-forgery" | "record-return-wrong-nominal" |
            "record-return-handle-as-int" => Some("return type"),
            "record-return-mutable-identity" => Some("immutable scalar/String fields"),
            "record-return-borrowed-array-owner" | "record-return-incoming-borrow" |
            "record-return-moved-local" => Some("record return requires"),
            "record-return-not-os-status" => Some("OS entrypoint requires"),
            "record-return-unknown-type" => Some("unknown record class"),
            _ => None,
        };
        if let Some(boundary) = expected_boundary {
            assert!(String::from_utf8_lossy(&rejected.stderr).contains(boundary),
                "{name}: wrong boundary: {}", String::from_utf8_lossy(&rejected.stderr));
        }
        if *name == "actor-string-region-boundary" {
            assert!(
                String::from_utf8_lossy(&rejected.stderr)
                    .contains("native strings currently require"),
                "{name}: wrong rejection: {}",
                String::from_utf8_lossy(&rejected.stderr)
            );
        }
        assert!(
            !String::from_utf8_lossy(&rejected.stderr)
                .to_lowercase()
                .contains("internal"),
            "{name}: internal compiler failure"
        );
    }
    let mut interpolation_bomb =
        "pub fnc main():int{val string x=`".to_owned();
    for _ in 0..1025 { interpolation_bomb.push_str("${\"x\"}"); }
    interpolation_bomb.push_str("`;return 0;}");

    let brace_bomb = format!(
        "pub fnc main():int{{val string x=`${{{}0{}}}`;return 0;}}",
        "{".repeat(257),
        "}".repeat(257),
    );

    let mut nested = "\"x\"".to_owned();
    for _ in 0..34 { nested = format!("`${{{nested}}}`"); }
    let nesting_bomb = format!(
        "pub fnc main():int{{val string x=`${{{nested}}}`;return 0;}}"
    );

    for (name, source, diagnostic) in [
        ("template-interpolation-limit", interpolation_bomb, "interpolation count limit exceeded"),
        ("template-brace-limit", brace_bomb, "brace nesting limit exceeded"),
        ("template-nesting-limit", nesting_bomb, "nesting limit exceeded"),
    ] {
        fs::write(scratch.0.join("main.ores"), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), foo).unwrap();
        let rejected = run(Command::new(&compiler).arg(scratch.0.join("main.ores")));
        assert_eq!(rejected.status.code(), Some(1), "{name}: must fail closed");
        assert!(rejected.stdout.is_empty(), "{name}: emitted partial IR");
        assert!(
            String::from_utf8_lossy(&rejected.stderr).contains(diagnostic),
            "{name}: wrong diagnostic: {}",
            String::from_utf8_lossy(&rejected.stderr)
        );
    }
    // Native module admission must bind to the opened regular-file inode.
    // FIFOs must not block compilation; a hard-link alias must not bypass
    // self-import detection. Ordinary regular symlinks remain supported.
    #[cfg(unix)]
    {
        use std::os::unix::fs::symlink;
        let entry = scratch.0.join("main.ores");
        let imported = scratch.0.join("main-1.ores");
        let library = scratch.0.join("library.ores");
        fs::write(&entry, exact).unwrap();
        fs::remove_file(&imported).unwrap();

        let made_fifo = Command::new("mkfifo").arg(&imported).status().unwrap();
        assert!(made_fifo.success(), "could not prepare FIFO import test");
        run_bounded_no_ir(Command::new(&compiler).arg(&entry),
                          &scratch.0, "FIFO import");
        fs::remove_file(&imported).unwrap();

        fs::hard_link(&entry, &imported).unwrap();
        let rejected = run(Command::new(&compiler).arg(&entry));
        assert_eq!(rejected.status.code(), Some(1), "self hard-link import admitted");
        assert!(rejected.stdout.is_empty(), "self hard-link import emitted partial IR");
        fs::remove_file(&imported).unwrap();

        fs::write(&library, foo).unwrap();
        symlink("library.ores", &imported).unwrap();
        let allowed = run(Command::new(&compiler).arg(&entry));
        assert!(
            allowed.status.success() && !allowed.stdout.is_empty(),
            "regular symlink import should remain supported: {}",
            String::from_utf8_lossy(&allowed.stderr)
        );
        fs::remove_file(&imported).unwrap();
        fs::remove_file(&library).unwrap();
    }
    fs::write(
        scratch.0.join("main.ores"),
        r#"pub fnc main():int{stdio.print("host only");return 0;}"#,
    )
    .unwrap();
    for target in ["--target=wasm32", "--target=thumbv7m-none-eabi"] {
        let rejected = run(Command::new(&compiler)
            .arg(target)
            .arg(scratch.0.join("main.ores")));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{target}: unsupported string target must fail"
        );
        assert!(rejected.stdout.is_empty(), "{target}: emitted partial IR");
        let boundary = if target == "--target=wasm32" {
            "native strings currently require"
        } else {
            "cortex-m target rejects main, effects or unbounded functions"
        };
        assert!(
            String::from_utf8_lossy(&rejected.stderr).contains(boundary),
            "{target}: wrong rejection: {}",
            String::from_utf8_lossy(&rejected.stderr)
        );
    }
    assert!(!sanitize || guest_address_checks, "missing guest ASan instrumentation proof");
    if require_java {
        assert_eq!(strict_comparisons, JAVA_LLVM_STRICT_NINE.len(), "exactly nine shared-source Java executions required");
        assert_eq!(case_evidence.len(), JAVA_LLVM_STRICT_NINE.len(), "each shared-source fixture requires receipt evidence");
        let mut receipt = fs::OpenOptions::new()
            .write(true).create_new(true)
            .open(receipt_path.expect("validated --report"))
            .expect("parity receipt must be a new file");
        let body = format!(
            r#"{{"schema":"oreslang.java-llvm.source-parity.v1","status":"passed","llvm_revision":"{}","java_revision":"{}","positive_programs":{},"native_executions":{},"java_executions":{},"llvm_only_negative_rejections":{},"sanitized":{},"cases":[{}]}}"#,
            llvm_revision.expect("validated --llvm-revision"),
            java_revision.expect("validated --java-revision"),
            strict_comparisons,
            strict_comparisons * 5,
            strict_comparisons * 2,
            negatives.len() + 2,
            sanitize,
            case_evidence.join(",")
        );
        receipt.write_all(body.as_bytes()).expect("write parity receipt");
        receipt.write_all(b"\n").expect("finish parity receipt");
        receipt.sync_all().expect("sync parity receipt");
    }
    if guest_address_checks { println!("PASS generated guest IR includes ASan memory checks"); }
    println!("PASS class/string parity: {} common-source positives, {} native-only positives, {} controlled-panic cases (five native executions each), {} no-IR rejections plus two target boundaries and two Unix filesystem boundary rejections, Java={comparisons}, standalone={standalone_runs}, sanitized={sanitize}", positives.len(), native_only.len(), panics.len(), negatives.len());
}
