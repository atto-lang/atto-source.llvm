//! Real .atto -> oreslang-llvmc -> LLVM IR -> clang -> OS exit verification.
 //! Positive scalar-only stack shape inference. Requires executed compiler and
 //! linker; this is NOT a host-only contract or an aggregate parameter ABI.
use std::fs::{self, read_dir};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

struct TempDir(PathBuf);
impl Drop for TempDir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}
fn temp() -> TempDir {
    let n = SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos();
    let root = std::env::temp_dir().join(format!("attolang-infer-{}-{n}",std::process::id()));
    fs::create_dir(&root).expect("isolated temp directory");
    TempDir(root)
}
fn run(binary:&Path, args:&[&Path], dir:&Path)->std::process::Output {
    Command::new(binary).args(args).current_dir(dir).output().expect("run actual binary")
}
fn main(){
    let compiler=PathBuf::from(std::env::args_os().nth(1).expect("compiler path"))
        .canonicalize().expect("actual compiler");
    let clang=PathBuf::from(std::env::args_os().nth(2).expect("clang path"))
        .canonicalize().expect("actual linker");
    let corpus=[
      ("infer-basic",r#"pub fnc main():int{
        const point = infer struct{x:20,y:22};
        return point.x + point.y;
      }"#,42),
      ("infer-spread",r#"pub fnc main():int{
        const base = infer struct{x:20};
        const next = infer struct{...base,y:22};
        return next.x + next.y;
      }"#,42),
      ("infer-record",r#"pub fnc main():int{
        const first = infer record{x:20};
        const second = infer record{...first,y:22};
        return second.x + second.y;
      }"#,42),
      ("override-type",r#"pub fnc main():int{
        const first = infer struct{x:true,y:40};
        const last = infer struct{...first,x:2};
        return last.x + last.y;
      }"#,42),
      ("computed-fields",r#"pub fnc main():int{
        const first = infer struct{x:20+2,y:20};
        return first.x + first.y;
      }"#,42),
      ("prior-scalar-local",r#"pub fnc main():int{
        const int initial = 20;
        const row = infer struct{x:initial,y:22};
        return row.x + row.y;
      }"#,42),
      ("original-unchanged",r#"pub fnc main():int{
        const old = record{x:20};
        const new_row = record{...old,x:42};
        return new_row.x - old.x + 20;
      }"#,42)
    ];
    const rejected=[
      ("missing spread source",r#"pub fnc main():int{
        const r=infer struct{...missing,y:2}; return r.y;
      }"#),
      ("duplicate named field",r#"pub fnc main():int{
        const r=infer struct{x:1,x:2}; return r.x;
      }"#),
      ("scalar spread operand",r#"pub fnc main():int{
        const r=infer record{...42,y:2}; return r.y;
      }"#),
      ("implicit aggregate escape",r#"pub fnc main():int{
        const r=infer struct{x:42}; return r;
      }"#),
      ("bool result returned as int",r#"pub fnc main():int{
        const r=infer struct{x:1}; const s=infer struct{...r,x:true};
        return s.x;
      }"#)
    ];
    for (name, source) in rejected {
        let work=temp();
        let src=work.0.join("negative.atto");
        fs::write(&src,source).unwrap();
        let result=run(&compiler,&[src.as_path()],&work.0);
        assert!(result.status.code().unwrap_or(-1)>0,
            "{name}: unsupported source was compiled or crashed");
        assert!(result.stdout.is_empty(),"{name}: rejected source emitted LLVM IR");
        assert!(!result.stderr.is_empty(),"{name}: missing diagnostic");
        assert_eq!(fs::read_to_string(&src).unwrap(),source,
            "{name}: compiler modified negative source");
        assert_eq!(read_dir(&work.0).unwrap().count(),1,
            "{name}: compiler published an artifact on rejection");
    }
    for (name, source, expected) in corpus {
        let work=temp();
        let src=work.0.join("program.atto");
        let ir=work.0.join("program.ll");
        let exe=work.0.join("program");
        fs::write(&src,source).unwrap();
        let output=run(&compiler,&[src.as_path()],&work.0);
        assert!(output.status.success(),"{}: compile error: {}",name,String::from_utf8_lossy(&output.stderr));
        assert!(output.stdout.starts_with(b"; ModuleID"),"{name}: compiler emitted no LLVM IR");
        assert!(output.stderr.is_empty(),"{name}: unexpected compiler diagnostic");
        assert_eq!(fs::read_to_string(&src).unwrap(),source,"{name}: source changed");
        assert_eq!(read_dir(&work.0).unwrap().count(),1,"{name}: compiler wrote a file");
        fs::write(&ir,output.stdout).unwrap();
        let linked=run(&clang,&[Path::new("-O0"),Path::new("-x"),Path::new("ir"),ir.as_path(),Path::new("-o"),exe.as_path()],&work.0);
        assert!(linked.status.success(),"{name}: clang linker failed: {}",String::from_utf8_lossy(&linked.stderr));
        let executed=run(&exe,&[],&work.0);
        assert_eq!(executed.status.code(),Some(expected),"{name}: guest exit != {expected}");
        assert!(executed.stdout.is_empty() && executed.stderr.is_empty(),"{name}: guest emitted output");
    }
    println!("real native inferred aggregate guest execution: {} positive, {} negative",corpus.len(),rejected.len());
}
