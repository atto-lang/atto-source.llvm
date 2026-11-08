// Native float source -> IR -> Clang -> executed guest, compared byte for byte
// with stdout captured from the Java/Graal backend at a pinned revision.
// Rust std only: no Python harness, no host numerics standing in for the guest.
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

struct Workdir(PathBuf);
impl Workdir {
    fn new() -> Self {
        let nonce = SystemTime::now().duration_since(UNIX_EPOCH).expect("clock").as_nanos();
        let path = std::env::temp_dir().join(format!("oreslang-native-float-{}-{}", std::process::id(), nonce));
        fs::create_dir(&path).expect("create workdir");
        Self(path)
    }
}
impl Drop for Workdir {
    fn drop(&mut self) { let _ = fs::remove_dir_all(&self.0); }
}

struct Tools { compiler: String, clang: String, driver: String, fixtures: PathBuf, dir: Workdir }

impl Tools {
    fn build(&self, source: &Path, id: &str) -> PathBuf {
        let binary = self.dir.0.join(id);
        let built = Command::new("bash").arg(&self.driver).arg(source).arg(&binary)
            .env("ORESLANG_LLVMC", &self.compiler).env("CLANG", &self.clang)
            .output().expect("run AOT driver");
        assert!(built.status.success(), "{id}: AOT build failed: {}", String::from_utf8_lossy(&built.stderr));
        binary
    }
    // The console runtime requires a pipe or terminal; a regular file fails closed.
    fn run(&self, binary: &Path) -> Output {
        Command::new(binary).stdout(Stdio::piped()).stderr(Stdio::piped()).output().expect("run guest")
    }
    fn reject(&self, id: &str, source: &str, diagnostic: &str) {
        let path = self.dir.0.join(format!("{id}.ores"));
        fs::write(&path, source).expect("write source");
        self.reject_path(id, &path, diagnostic);
    }
    fn reject_path(&self, id: &str, path: &Path, diagnostic: &str) {
        let out = Command::new(&self.compiler).arg("--target=host").arg(path).output().expect("run compiler");
        let stderr = String::from_utf8_lossy(&out.stderr);
        assert_eq!(out.status.code(), Some(1), "{id}: expected source rejection, got {:?}: {stderr}", out.status.code());
        assert!(out.stdout.is_empty(), "{id}: rejected source emitted IR");
        assert!(stderr.starts_with("oreslang-llvmc: ") && stderr.contains(diagnostic),
            "{id}: expected diagnostic {diagnostic:?}, got: {stderr}");
    }
}

fn expect_guest(tools: &Tools, relative: &str, code: i32) -> Output {
    let source = tools.fixtures.join(format!("{relative}.ores"));
    let id = relative.replace('/', "-");
    let out = tools.run(&tools.build(&source, &id));
    assert_eq!(out.status.code(), Some(code), "{relative}: exit status; stderr: {}", String::from_utf8_lossy(&out.stderr));
    out
}

fn expect_java_stdout(tools: &Tools, relative: &str, code: i32) -> Output {
    let out = expect_guest(tools, relative, code);
    let expected = fs::read(tools.fixtures.join(format!("{relative}.expected"))).expect("expected stdout");
    assert!(out.stdout == expected, "{relative}: stdout differs from Java oracle\n--- native\n{}\n--- java\n{}",
        String::from_utf8_lossy(&out.stdout), String::from_utf8_lossy(&expected));
    out
}

fn main() {
    let mut args = std::env::args().skip(1);
    let usage = "usage: native-float-ml compiler clang aot-driver fixtures";
    let tools = Tools {
        compiler: args.next().expect(usage),
        clang: args.next().expect(usage),
        driver: args.next().expect(usage),
        fixtures: PathBuf::from(args.next().expect(usage)),
        dir: Workdir::new(),
    };
    assert!(args.next().is_none(), "{usage}");

    for relative in ["numerics", "linear/train", "neuron", "control", "aggregates"] {
        expect_java_stdout(&tools, relative, 0);
    }

    // rt fail: admitted output is flushed first, then status 1 and the message.
    let failed = expect_java_stdout(&tools, "rt-fail", 1);
    assert!(String::from_utf8_lossy(&failed.stderr).contains("non-positive input"), "rt fail message missing");

    // Long-running loops must not exhaust the record region or the output queue.
    let long = expect_guest(&tools, "long-run", 0);
    let mut expected = String::from("checkpoint\ncheckpoint\ncheckpoint\ncheckpoint\n100000.0\n200000\n");
    for i in 0..70000 { expected.push_str(&format!("{i}\n")); }
    expected.push_str("done\n");
    assert!(long.stdout == expected.as_bytes(), "long-run: stdout differs ({} bytes)", long.stdout.len());

    // Java type errors stay compile errors: nothing narrows implicitly.
    let main_with = |body: &str| format!("pub routine main(): void {{ {body} }}\n");
    for (id, body, diagnostic) in [
        ("float_to_int", "val int x = 1.5; stdio.stdout.println(x);", "requires int, got f64"),
        ("int_div_to_int", "val int x = 1 / 2; stdio.stdout.println(x);", "requires int, got f64"),
        ("float_to_bool", "val bool b = 1.5; stdio.stdout.println(b);", "requires bool, got f64"),
        ("bool_to_float", "val float f = true; stdio.stdout.println(f);", "requires f64, got bool"),
        ("string_plus_float", "val String s = \"a\" + 1.5; stdio.stdout.println(s);", "requires string, got f64"),
        ("bool_times_float", "stdio.stdout.println(true * 1.5);", "requires int or f64"),
        ("assign_float_to_int", "let int x = 1; x = 2.5; stdio.stdout.println(x);", "requires int, got f64"),
        ("fail_non_string", "rt fail(1);", "rt fail message requires string"),
        ("decimal_suffix", "val float x = 1.5d; stdio.stdout.println(x);", "decimal literal unsupported"),
        ("f32_requires_binary32", "val f32 x = 1.0; stdio.stdout.println(x);", "native f32 requires binary32 lowering"),
        ("float_remainder_reserved", "stdio.stdout.println(5.5 % 2.0);", "native remainder requires int operands"),
    ] {
        tools.reject(id, &main_with(body), diagnostic);
    }
    tools.reject("float_arg_to_int", "fnc f(int x): int { return x; }\npub routine main(): void { stdio.stdout.println(f(1.5)); }\n", "requires int, got f64");
    tools.reject("float_ret_from_int", "fnc f(): int { return 1.5; }\npub routine main(): void { stdio.stdout.println(f()); }\n", "requires int, got f64");
    tools.reject("float_main", "pub routine main(): float { return 1.0; }\n", "native main does not admit f64 process ABI");
    tools.reject("float_field_from_bool", "define class C as\n  pub val float a;\nend\npub routine main(): void { val C c = new C(true); stdio.stdout.println(c.a); }\n", "requires f64, got bool");

    // Native rt fail ends the process, so it cannot coexist with cleanup or a
    // handler that the Java backend would run during unwinding.
    tools.reject("fail_with_defer",
        "fnc g(): int { rt fail(\"x\"); return 1; }\ndefine class C as\n  pub let int v = 1;\nend\nfnc h(borrow mut C c): int { defer c.v = 2; return 1; }\npub routine main(): void { let mut C c = new C(); stdio.stdout.println(h(rt mut borrow c)); stdio.stdout.println(g()); }\n",
        "native rt fail requires unwind lowering");
    tools.reject("fail_call_inside_try",
        "fnc g(): int { rt fail(\"x\"); return 1; }\nfnc h(): int { try { return g(); } catch (e) { return 0; } finally { } }\npub routine main(): void { stdio.stdout.println(h()); }\n",
        "native protected try return requires nonthrowing");

    // Control flow and visibility stay checked.
    tools.reject("module_private_call", "define module m as\n  fnc secret(): int { return 1; }\nend\npub routine main(): void { stdio.stdout.println(m.secret()); }\n", "requires a public member");
    tools.reject("break_in_if_outside_loop", "pub routine main(): void { if true then break; fi }\n", "break/continue requires an enclosing loop");
    tools.reject("rebind_mutable_class", "define class C as\n  pub let int a = 1;\nend\npub routine main(): void { let C c = new C(); c = new C(); stdio.stdout.println(c.a); }\n", "assignment requires");
    tools.reject("print_class_value", "define class C as\n  pub val float a;\nend\npub routine main(): void { val C c = new C(1.0); stdio.stdout.println(c); }\n", "class is non-Copy");

    // import fnc selects public free functions only.
    let lib = tools.dir.0.join("lib.ores");
    fs::write(&lib, "fnc secret(): float { return 1.0; }\npub fnc open(): float { return secret(); }\n").expect("write lib");
    for (id, source, diagnostic) in [
        ("import_private_fnc", "import fnc secret from './lib';\npub routine main(): void { stdio.stdout.println(1); }\n", "unknown or private imported function"),
        ("import_unknown_fnc", "import fnc absent from './lib';\npub routine main(): void { stdio.stdout.println(1); }\n", "unknown or private imported function"),
        ("import_redefined_fnc", "import fnc open from './lib';\nfnc open(): float { return 2.0; }\npub routine main(): void { stdio.stdout.println(open()); }\n", "conflicts with local function"),
    ] {
        tools.reject(id, source, diagnostic);
    }
    println!("NATIVE_FLOAT_ML_PASS");
}
