//! Execute real LLVM-generated machine code, optionally comparing Java output.
use std::{
    env, fs,
    path::PathBuf,
    process::{Command, Output},
    time::{SystemTime, UNIX_EPOCH},
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
fn main() {
    let (mut compiler, mut clang, mut runtime, mut java) = (None, None, None, None);
    let mut sanitize = false;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--compiler" => compiler = args.next(),
            "--clang" => clang = args.next(),
            "--runtime" => runtime = args.next(),
            "--java-command" => java = args.next(),
            "--sanitize" => sanitize = true,
            _ => panic!("unknown argument {arg}"),
        }
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
        ("exact-user-program", exact, "hello world\nhiiii 2hiiii"),
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
    let mut comparisons = 0;
    for (name, source, expected) in &positives {
        fs::write(scratch.0.join("main.ores"), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), foo).unwrap();
        let path = scratch.0.join("main.ores");
        let generated = run(Command::new(&compiler).arg(&path));
        assert!(
            generated.status.success() && !generated.stdout.is_empty(),
            "{name}: {}",
            String::from_utf8_lossy(&generated.stderr)
        );
        let ir = scratch.0.join("program.ll");
        let binary = scratch.0.join("program");
        fs::write(&ir, generated.stdout).unwrap();
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
            .arg(&runtime)
            .arg("-pthread")
            .arg("-o")
            .arg(&binary));
        assert!(
            linked.status.success() && binary.is_file(),
            "{name}: {}",
            String::from_utf8_lossy(&linked.stderr)
        );
        for _ in 0..5 {
            let native = run(&mut Command::new(&binary));
            assert_eq!(
                native.status.code(),
                Some(0),
                "{name}: {}",
                String::from_utf8_lossy(&native.stderr)
            );
            assert_eq!(native.stdout, expected.as_bytes(), "{name}: native output");
            assert!(
                native.stderr.is_empty(),
                "{name}: sanitizer/runtime diagnostics: {}",
                String::from_utf8_lossy(&native.stderr)
            );
        }
        if let Some(launcher) = &java {
            let reference = run(Command::new(launcher).arg(&path));
            assert_eq!(
                reference.status.code(),
                Some(0),
                "Java {name}: {}",
                String::from_utf8_lossy(&reference.stderr)
            );
            assert_eq!(reference.stdout, expected.as_bytes(), "Java {name}: output");
            comparisons += 1;
        }
    }
    let negatives = [
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
        fs::write(scratch.0.join("main.ores"), source).unwrap();
        fs::write(scratch.0.join("main-1.ores"), dependency).unwrap();
        let rejected = run(Command::new(&compiler).arg(scratch.0.join("main.ores")));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{name}: must fail before codegen"
        );
        assert!(rejected.stdout.is_empty(), "{name}: emitted partial IR");
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
            "cortex-m target rejects main, effects or non-scalar functions"
        };
        assert!(
            String::from_utf8_lossy(&rejected.stderr).contains(boundary),
            "{target}: wrong rejection: {}",
            String::from_utf8_lossy(&rejected.stderr)
        );
    }
    println!("PASS class/string parity: {} positives (five native executions each), {} no-IR rejections plus two target boundaries, Java={comparisons}, sanitized={sanitize}", positives.len(), negatives.len());
}
