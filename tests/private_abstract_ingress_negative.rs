// Compiler-backed negative source matrix; NO host-only or source-free pass counts.
// This test must run the exact built oreslang-llvmc from the PR head.
use std::fs;
use std::path::Path;
use std::process::Command;
use std::time::{SystemTime, UNIX_EPOCH};

const ABSTRACT: &str = "NATIVE_PRIVATE_ABSTRACT_UNSUPPORTED";
const INGRESS: &str = "NATIVE_ACTOR_INGRESS_UNSUPPORTED";

struct Case {
    name: &'static str,
    source: &'static str,
    diagnostic: &'static str,
}

fn compile(compiler: &Path, path: &Path) -> std::process::Output {
    Command::new(compiler).arg(path).output().expect("launch exact-head native compiler")
}

fn main() {
    let compiler = std::env::args_os().nth(1).expect("compiler path argument required");
    let compiler = std::path::PathBuf::from(compiler);
    assert!(compiler.is_file(), "missing built compiler: {}", compiler.display());
    let stamp = SystemTime::now().duration_since(UNIX_EPOCH).expect("clock").as_nanos();
    let dir = std::env::temp_dir().join(format!("atto-private-abstract-{}-{}", std::process::id(), stamp));
    fs::create_dir(&dir).expect("create isolated fixture directory");
    let good = dir.join("positive.ores");
    fs::write(&good, "pub fnc main(): int { return 42; }\n").expect("write positive control");
    let result = compile(&compiler, &good);
    assert!(result.status.success(), "scalar positive control failed: {}", String::from_utf8_lossy(&result.stderr));
    assert!(String::from_utf8_lossy(&result.stdout).contains("ModuleID"), "positive control emitted no LLVM IR");

    let cases = [
        Case { name: "abstract-class", source: "define abstract class Base as\n private abstract mtd hook(): int;\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "class-private-abstract", source: "define class Base as\n private abstract mtd hook(): int;\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "class-abstract", source: "define class Base as\n abstract mtd hook(): int;\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "class-pub-abstract", source: "define class Base as\n pub abstract mtd hook(): int;\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "class-inherits", source: "define class Child extends Base as\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "class-from-parent", source: "define class Child from Base as\nend\npub fnc main(): int { return 0; }\n", diagnostic: ABSTRACT },
        Case { name: "actor-implicit-self-receive", source: "define actor Worker as\n let count: int = 0;\n receive(ActorMail<int> mail): void { return; }\nend\npub fnc main(): int { return 0; }\n", diagnostic: INGRESS },
        Case { name: "actor-explicit-self-receive", source: "define actor Worker as\n let count: int = 0;\n receive(self, ActorMail<int> mail): void { return; }\nend\npub fnc main(): int { return 0; }\n", diagnostic: INGRESS },
        Case { name: "actor-pub-receive", source: "define actor Worker as\n let count: int = 0;\n pub receive(ActorMail<int> mail): void { return; }\nend\npub fnc main(): int { return 0; }\n", diagnostic: INGRESS },
        Case { name: "actor-private-abstract", source: "define actor Worker as\n let count: int = 0;\n private abstract receive(ActorMail<int> mail): void;\nend\npub fnc main(): int { return 0; }\n", diagnostic: INGRESS },
        Case { name: "actor-external-receive", source: "define actor Worker as\n let count: int = 0;\n external mtd receive(ActorMail<int> mail): void { return; }\nend\npub fnc main(): int { return 0; }\n", diagnostic: INGRESS },
    ];
    for case in &cases {
        let path = dir.join(format!("{}.ores", case.name));
        fs::write(&path, case.source).expect("write negative source");
        let result = compile(&compiler, &path);
        assert!(!result.status.success(), "{} was improperly admitted", case.name);
        assert!(result.stdout.is_empty(), "{} emitted LLVM IR/stdout on rejection", case.name);
        assert!(String::from_utf8_lossy(&result.stderr).contains(case.diagnostic),
            "{} gave wrong failure (expected {}): {}", case.name, case.diagnostic, String::from_utf8_lossy(&result.stderr));
    }
    let count = fs::read_dir(&dir).expect("list fixture output").count();
    assert_eq!(count, cases.len() + 1, "compiler left unexpected staged artifacts");
    fs::remove_dir_all(&dir).expect("remove isolated fixtures");
    println!("PASS: exact compiler scalar IR positive + {} negative no-IR admissions; NOT private-abstract or actor hook positive parity", cases.len());
}
