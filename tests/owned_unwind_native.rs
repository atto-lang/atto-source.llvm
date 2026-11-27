use std::{fs,process::Command};
const D:&str=include_str!("fixtures/owned-pending-defer.atto");
const R:&str=include_str!("fixtures/owned-pending-recover.atto");
fn run(c:&mut Command){let o=c.output().unwrap();assert!(o.status.success(),"{} {}",String::from_utf8_lossy(&o.stdout),String::from_utf8_lossy(&o.stderr));}
#[test]
fn source_to_native_and_negative_admission(){
 let root=std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"));
 let dir=std::env::temp_dir().join(format!("atto-owned-native-{}",std::process::id()));fs::create_dir(&dir).unwrap();
 struct Cleanup(std::path::PathBuf);impl Drop for Cleanup{fn drop(&mut self){let _=fs::remove_dir_all(&self.0);}}let _cleanup=Cleanup(dir.clone());
 let cli=env!("CARGO_BIN_EXE_atto-llvm");
 for (name,source) in [("defer",D),("recover",R)]{
  let src=dir.join("input.atto");fs::write(&src,source).unwrap();let ir=dir.join("guest.ll");
  run(Command::new(cli).arg(&src).arg("-o").arg(&ir).arg("--owned-unwind-v1"));
  let expected=fs::read(&ir).unwrap();
  for a in ["susp","⧖","∂"]{fs::write(&src,source.replace("suspends",a)).unwrap();run(Command::new(cli).arg(&src).arg("-o").arg(&ir).arg("--owned-unwind-v1"));assert_eq!(fs::read(&ir).unwrap(),expected);}
  for bad in [source.replace("take! ",""),source.replace("take! Future","take!Future"),source.replace("take! Future","take ! Future"),source.replace("suspends ",""),source.replace("Future<int>","Future<bool>"),format!("{source} pub fnc main():int{{return 0;}}")]{
   fs::write(&src,bad).unwrap();let p=Command::new(cli).arg(&src).arg("-o").arg(&ir).arg("--owned-unwind-v1").output().unwrap();assert!(!p.status.success());assert_eq!(fs::read(&ir).unwrap(),expected);
  }
  let exe=dir.join("guest");
  for opt in ["-O0","-O2"]{
   run(Command::new("clang").arg(opt).args(["-fsanitize=address,undefined","-fno-sanitize-recover=all"]).arg(&ir).arg(root.join("tests/owned_unwind_native_harness.c")).arg("-o").arg(&exe));
   run(Command::new(&exe).arg(name));
   if name=="recover"{
    let runtime=root.join("src/main/cpp/oreslang/runtime");
    run(Command::new("clang").arg(opt).args(["-fsanitize=address,undefined","-fno-sanitize-recover=all","-pthread"]).arg("-I").arg(root.join("src/main/cpp")).arg(&ir).arg(root.join("tests/owned_unwind_store_harness.c")).arg(runtime.join("NativeExecutionContextStore.c")).arg(runtime.join("NativeContinuationStore.c")).arg("-o").arg(&exe));
    run(&mut Command::new(&exe));
   }
  }
 }
}
