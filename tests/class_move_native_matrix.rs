//! Execute native move/borrow CFG proofs with assertions retained under rustc -O.
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
    let mut compiler = None;
    let mut clang = None;
    let mut sanitize = false;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--probe-assertions" => assert!(
                std::hint::black_box(false),
                "optimized Rust assertions remain enabled"
            ),
            "--self-test" => {
                let probe =
                    run(Command::new(env::current_exe().unwrap()).arg("--probe-assertions"));
                assert!(!probe.status.success());
                assert!(String::from_utf8_lossy(&probe.stderr)
                    .contains("optimized Rust assertions remain enabled"));
                println!("PASS optimized Rust assertion self-test");
                return;
            }
            "--compiler" => compiler = args.next(),
            "--clang" => clang = args.next(),
            "--sanitize" => sanitize = true,
            _ => panic!("unknown argument {arg}"),
        }
    }
    let compiler = compiler.expect("--compiler");
    let clang = clang.expect("--clang");
    let root = Scratch(
        env::temp_dir().join(format!(
            "ores-class-moves-{}-{}",
            std::process::id(),
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        )),
    );
    fs::create_dir(&root.0).unwrap();
    for (name, source) in POSITIVE {
        let path = root.0.join(format!("{name}.ores"));
        fs::write(&path, source).unwrap();
        let generated = run(Command::new(&compiler).arg(&path));
        assert!(
            generated.status.success() && !generated.stdout.is_empty(),
            "{name}: {}",
            String::from_utf8_lossy(&generated.stderr)
        );
        let ir = String::from_utf8(generated.stdout).unwrap();
        assert!(
            !ir.contains("gc.statepoint") && !ir.contains("gc.root"),
            "{name}: unexpected tracing GC IR"
        );
        let ll = root.0.join(format!("{name}.ll"));
        fs::write(&ll, ir).unwrap();
        let binary = root.0.join(name);
        let mut link = Command::new(&clang);
        link.arg("-O1");
        if sanitize {
            link.args([
                "-fsanitize=address,undefined",
                "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer",
            ]);
        }
        let built = run(link.args(["-x", "ir"]).arg(&ll).arg("-o").arg(&binary));
        assert!(
            built.status.success(),
            "{name}: {}",
            String::from_utf8_lossy(&built.stderr)
        );
        let native = run(&mut Command::new(&binary));
        assert_eq!(
            native.status.code(),
            Some(42),
            "{name}: {}",
            String::from_utf8_lossy(&native.stderr)
        );
        assert!(
            native.stderr.is_empty(),
            "{name}: native/sanitizer diagnostics: {}",
            String::from_utf8_lossy(&native.stderr)
        );
    }
    for (name, source, diagnostic) in NEGATIVE {
        let path = root.0.join(format!("{name}.ores"));
        fs::write(&path, source).unwrap();
        let rejected = run(Command::new(&compiler).arg(&path));
        assert_eq!(
            rejected.status.code(),
            Some(1),
            "{name}: invalid source must reject"
        );
        assert!(
            rejected.stdout.is_empty(),
            "{name}: invalid source emitted partial IR"
        );
        assert!(
            String::from_utf8_lossy(&rejected.stderr).contains(diagnostic),
            "{name}: expected {diagnostic}: {}",
            String::from_utf8_lossy(&rejected.stderr)
        );
    }
    println!(
        "PASS: {} native class moves, {} no-IR rejections, sanitized={sanitize}",
        POSITIVE.len(),
        NEGATIVE.len()
    );
}

const POSITIVE: &[(&str, &str)] = &[
    (
        "independent-owner-survives-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair independent = new Pair();
  let mut Pair dest = rt take(source);
  return dest.a + independent.b;
}
"####,
    ),
    (
        "partial-field-move-keeps-sibling",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  const moved: int = rt take source.a;
  return moved + source.b;
}
"####,
    ),
    (
        "move-parenthesized",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  return dest.a + dest.b;
}
"####,
    ),
    (
        "move-unparenthesized",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take source;
  return dest.a + dest.b;
}
"####,
    ),
    (
        "multi-hop-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair second = rt take(source);
  let mut Pair third = rt take(second);
  return third.a + third.b;
}
"####,
    ),
    (
        "borrow-after-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

fnc plus_one(borrow mut Pair pair): void {
  pair.b = 30;
  return;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  plus_one(rt mut borrow dest);
  return dest.a + dest.b - 1;
}
"####,
    ),
    (
        "guarded-move-taken",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a == 13) { return second.b + 13; } fi
  return second.a + second.b;
}
"####,
    ),
    (
        "guarded-move-fallthrough",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a eq 0) { return 0; } fi
  return second.a + second.b;
}
"####,
    ),
    (
        "guarded-scalar-branch-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  const x: int = 42;
  let mut Pair first = new Pair();
  let mut Pair second = rt take(first);
  if (second.a eq 13) { return rt take x; } fi
  return x;
}
"####,
    ),
];
const NEGATIVE: &[(&str, &str, &str)] = &[
    (
        "whole-move-after-partial-field-take",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  const moved: int = rt take source.a;
  let Pair dest = rt take(source);
  return dest.b + moved;
}
"####,
        "ownership violation",
    ),
    (
        "borrow-after-whole-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

fnc view(borrow Pair candidate): int {
  return candidate.a;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  let Pair dest = rt take(source);
  return view(rt borrow source) + dest.b;
}
"####,
        "ownership violation",
    ),
    (
        "guard-read-after-whole-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let Pair dest = rt take(source);
  if (source.a eq 13) { return 0; } fi
  return dest.a + dest.b;
}
"####,
        "ownership violation",
    ),
    (
        "use-after-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  return source.a + dest.b;
}
"####,
        "ownership violation: use-after-move",
    ),
    (
        "double-move",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = rt take(source);
  let mut Pair again = rt take(source);
  return dest.a + again.b;
}
"####,
        "ownership violation: class move requires live unborrowed owner",
    ),
    (
        "wrong-type",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end
define class Other as
  pub let int a = 10;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let Other dest = rt take(source);
  return dest.a;
}
"####,
        "ownership violation: class move type mismatch",
    ),
    (
        "borrowed-source",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

fnc steal(borrow Pair source): int {
  let mut Pair dest = rt take(source);
  return dest.a;
}
pub fnc main(): int {
  let mut Pair source = new Pair();
  return steal(rt borrow source);
}
"####,
        "ownership violation: class move requires live unborrowed owner",
    ),
    (
        "unknown-owner",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair dest = rt take(not_defined);
  return dest.a;
}
"####,
        "unknown class move source",
    ),
    (
        "implicit-copy",
        r####"define class Pair as
  pub let int a = 13;
  pub let int b = 29;
end

pub fnc main(): int {
  let mut Pair source = new Pair();
  let mut Pair dest = source;
  return dest.a;
}
"####,
        "expected 'rt'",
    ),
];
