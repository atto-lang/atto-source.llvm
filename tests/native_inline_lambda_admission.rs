//! Actual LLVM IR -> clang native executable, restricted tuple profile.
//! Shared scalar corpus blob 193249523cf18ef725f4900c30f3925c5bf8ce4b.
use std::{fs,path::{Path,PathBuf},process::{Command,Output},time::{SystemTime,UNIX_EPOCH}};
struct Temp(PathBuf);
impl Drop for Temp {fn drop(&mut self){let _=fs::remove_dir_all(&self.0);}}
fn compile(compiler:&Path,dir:&Path,source:&str)->Output {
    fs::create_dir(dir).unwrap();
    let path=dir.join("probe.atto");
    fs::write(&path,source).unwrap();
    let r=Command::new(compiler).arg(&path).current_dir(dir).output().expect("real LLVM compiler");
    assert_eq!(fs::read_to_string(&path).unwrap(),source);
    assert_eq!(fs::read_dir(dir).unwrap().count(),1,"compiler emitted unexpected artifacts");
    r
}
fn main(){
    let compiler=PathBuf::from(std::env::args_os().nth(1).expect("compiler")).canonicalize().unwrap();
    let clang=std::env::args_os().nth(2).unwrap_or_else(||"clang".into());
    let dir=Temp(std::env::temp_dir().join(format!("atto-llvm-tuple-{}-{}",std::process::id(),SystemTime::now().duration_since(UNIX_EPOCH).unwrap().as_nanos())));
    fs::create_dir(&dir.0).unwrap();
    #[cfg(unix)] {use std::os::unix::fs::PermissionsExt;fs::set_permissions(&dir.0,fs::Permissions::from_mode(0o700)).unwrap();}
    let positive=[
        ("flat-three", "pub fnc main(): int { return (5, 3, 5) |> |(a, b, c)| -> a + b + c; }\n", 13),
        ("order-weighted", "pub fnc main(): int { return (2, 7, 11) |> |(a, b, c)| -> 2*a + 3*b + 4*c; }\n", 69),
        ("discard-middle", "pub fnc main(): int { return (5, 99, 8) |> |(a, _, c)| -> a + c; }\n", 13),
        ("discard-first", "pub fnc main(): int { return (99, 8) |> |(_, b)| -> b + 3; }\n", 11),
        ("discard-last", "pub fnc main(): int { return (6, 99) |> |(a, _)| -> a + 5; }\n", 11),
        ("discard-all", "pub fnc main(): int { return (5, 8) |> |(_, _)| -> 19; }\n", 19),
        ("singleton", "pub fnc main(): int { return (9,) |> |(a,)| -> a; }\n", 9),
        ("empty", "pub fnc main(): int { return () |> |()| -> 7; }\n", 7),
        ("typed-leaves", "pub fnc main(): int { return (5, 8) |> |(a: int, b: int)| -> a + b; }\n", 13),
        ("parenthesized-stage", "pub fnc main(): int { return ((5, 8) |> |(a, b)| -> a + b) + 1; }\n", 14),
        ("nested-scalar-stage", "pub fnc main(): int { return (5, 8) |> |(a, b)| -> ((a, 2) |> |(x, y)| -> x*y) + b; }\n", 18),
        ("signed-values", "pub fnc main(): int { return (-5, 8, 12) |> |(a, b, c)| -> a + b + c; }\n", 15),
        ("maximum-arity", "pub fnc main(): int { return (1, 2, 3, 4, 5, 6, 7, 8) |> |(a,b,c,d,e,f,g,h)| -> a+b+c+d+e+f+g+h; }\n", 36),
        ("ordinary-pipe","fnc inc(int n): int { return n+1; } pub fnc main(): int { return 41 |> inc; }\n",42),
        ("comment-control","pub fnc main(): int { /* |(a,b)| -> */ return 42; }\n",42),
        ("logical-control","fnc logical(): bool { return true || false; } pub fnc main(): int { return 42; }\n",42),
        ("repeated-binding","pub fnc main(): int { return (5,8) |> |(a,_)| -> a+a; }\n",10),
        ("discard-name-collision","pub fnc main(): int { return (5,99) |> |(__atto_tuple_discard_1,_)| -> __atto_tuple_discard_1; }\n",5),
    ];
    for (name,source,want) in positive {
        let cwd=dir.0.join(name);let r=compile(&compiler,&cwd,source);
        assert!(r.status.success()&&r.stderr.is_empty(),"{name}: {:?}",r);
        let ir=String::from_utf8(r.stdout).unwrap();
        assert!(ir.contains("define i64 @__oreslang_guest_main("),"{name}: no guest IR");
        if source.contains("|(")&&!source.contains("/*") {assert!(ir.contains("define internal i64")&&ir.contains("atto_tuple_pattern"),"{name}: no private helper");}
        fs::write(cwd.join("guest.ll"),ir).unwrap();
        for opt in ["-O0","-O2"] {
            let exe=cwd.join(format!("guest{opt}"));
            let built=Command::new(&clang).arg(opt).arg(cwd.join("guest.ll")).arg("-o").arg(&exe).output().unwrap();
            assert!(built.status.success(),"{name} {opt}: {:?}",built);
            let guest=Command::new(&exe).output().unwrap();
            assert_eq!(guest.status.code(),Some(want),"{name} {opt}: {:?}",guest);
            assert!(guest.stdout.is_empty()&&guest.stderr.is_empty());
        }
    }
    let negative=[
        ("duplicate","(1,2) |> |(a,a)| -> a","NATIVE_TUPLE_PATTERN_DUPLICATE_BINDING"),
        ("arity","(1,2) |> |(a,b,c)| -> a","NATIVE_TUPLE_PATTERN_ARITY"),
        ("scalar","1 |> |(a,b)| -> a+b","NATIVE_INLINE_LAMBDA_UNSUPPORTED"),
        ("scalar-lambda","5 |> |a| -> a","NATIVE_INLINE_LAMBDA_UNSUPPORTED"),
        ("discard-read","(1,2) |> |(a,_)| -> _","NATIVE_TUPLE_PATTERN_BODY"),
        ("typed-discard","(1,2) |> |(_:int,b)| -> b","NATIVE_TUPLE_PATTERN_TYPED_DISCARD"),
        ("singleton","(1,) |> |(a)| -> a","NATIVE_TUPLE_PATTERN_SINGLETON"),
        ("capture","(1,2) |> |(a,b)| -> a+missing","NATIVE_TUPLE_PATTERN_BODY"),
        ("bool-root","(true,2) |> |(a,b)| -> a","NATIVE_TUPLE_PATTERN_TYPE"),
        ("escape","(1,2)","native tuple literal unsupported"),
        ("nested-tuple","((1,2),3) |> |(a,b)| -> b","native tuple literal unsupported"),
    ];
    for (name,expr,code) in negative {
        let source=format!("pub fnc main(): int {{ return {expr}; }}\n");
        let r=compile(&compiler,&dir.0.join(format!("negative-{name}")),&source);
        assert_eq!(r.status.code(),Some(1),"{name}: {:?}",r);
        assert!(r.stdout.is_empty()&&String::from_utf8_lossy(&r.stderr).contains(code),"{name}: {:?}",r);
    }
    for (name,expr) in [("first","(9223372036854775807+1,8) |> |(_,b)| -> b"),("last","(5,9223372036854775807+1) |> |(a,_)| -> a")] {
        let cwd=dir.0.join(format!("overflow-{name}"));
        let r=compile(&compiler,&cwd,&format!("pub fnc main(): int {{ return {expr}; }}\n"));
        assert!(r.status.success(),"{name}: {:?}",r);
        fs::write(cwd.join("guest.ll"),r.stdout).unwrap();
        for opt in ["-O0","-O2"] {
            let exe=cwd.join(format!("guest{opt}"));
            assert!(Command::new(&clang).arg(opt).arg(cwd.join("guest.ll")).arg("-o").arg(&exe).status().unwrap().success());
            let r=Command::new(exe).current_dir(&cwd).output().unwrap();
            #[cfg(unix)] {use std::os::unix::process::ExitStatusExt; assert_eq!(r.status.signal(),Some(6),"checked overflow did not abort: {r:?}");}
            assert!(!r.status.success()&&r.stdout.is_empty());
        }
    }
    println!("LLVM_TUPLE_NATIVE_PASS 13 shared + 5 extra positive at O0/O2, 11 negative and 4 discarded overflow outcomes");
}
