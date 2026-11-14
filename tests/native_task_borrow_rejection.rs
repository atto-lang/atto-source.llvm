use std::{
    env,
    fs,
    path::{Path, PathBuf},
    process::Command,
    time::{SystemTime, UNIX_EPOCH},
};

struct TempDir(PathBuf);

impl TempDir {
    fn new() -> Result<Self, String> {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(|e| format!("clock error: {e}"))?
            .as_nanos();
        let path = env::temp_dir().join(format!(
            "oreslang-native-task-borrow-{}-{nonce}",
            std::process::id()
        ));
        fs::create_dir(&path)
            .map_err(|e| format!("create temp dir {}: {e}", path.display()))?;
        Ok(Self(path))
    }

    fn path(&self) -> &Path {
        &self.0
    }
}

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

#[derive(Clone, Copy)]
struct Case {
    name: &'static str,
    source: &'static str,
    diagnostic: &'static str,
}

const CASES: &[Case] = &[
    Case {
        name: "task-captured-shared-loan",
        source: "pub fnc main(int x): int { return task { return rt borrow x; }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "task-captured-exclusive-loan",
        source: "pub fnc main(int x): int { return task { return rt mut borrow x; }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "task-borrowed-call-result",
        source: "fnc read(borrow int x): int { return x; } pub fnc main(int x): int { return task { return read(rt borrow x); }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "task-borrowed-classic-for-initializer",
        source: "pub fnc main(int owner): int { return task { for (val loop = rt borrow owner; false; ) {} return 0; }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "nested-task-borrow",
        source: "pub fnc main(int x): int { return task { return task { return rt borrow x; }; }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "task-comment-borrow",
        source: "pub fnc main(int x): int { return task /* comment */ { return rt borrow x; }; }",
        diagnostic: "native execution task lowering unsupported",
    },
    Case {
        name: "cpu-task-borrow",
        source: "pub fnc main(int x): int { return cpu task { return rt borrow x; }; }",
        diagnostic: "native cpu execution block boundary unsupported",
    },
    Case {
        name: "gpu-task-borrow",
        source: "pub fnc main(int x): int { return gpu task { return rt borrow x; }; }",
        diagnostic: "native gpu/quantum boundary unsupported",
    },
];

fn run_case(compiler: &Path, root: &Path, case: Case) -> Result<(), String> {
    let fixture = root.join(format!("{}.ores", case.name));
    fs::write(&fixture, format!("{}\n", case.source))
        .map_err(|e| format!("{}: write fixture: {e}", case.name))?;

    let output = Command::new(compiler)
        .arg(&fixture)
        .output()
        .map_err(|e| format!("{}: execute compiler: {e}", case.name))?;

    let stdout = String::from_utf8(output.stdout)
        .map_err(|e| format!("{}: compiler stdout was not UTF-8: {e}", case.name))?;
    let stderr = String::from_utf8(output.stderr)
        .map_err(|e| format!("{}: compiler stderr was not UTF-8: {e}", case.name))?;

    if output.status.success() {
        return Err(format!(
            "{}: unsupported borrowed Task was admitted with status {}",
            case.name, output.status
        ));
    }
    if !stdout.trim().is_empty() {
        return Err(format!(
            "{}: compiler emitted stdout/LLVM IR on rejection: {:?}",
            case.name, stdout
        ));
    }
    if !stderr.contains(case.diagnostic) {
        return Err(format!(
            "{}: expected diagnostic {:?}; stderr={:?}",
            case.name, case.diagnostic, stderr
        ));
    }
    Ok(())
}

fn main() {
    let mut args = env::args_os();
    let _program = args.next();
    let Some(compiler) = args.next() else {
        eprintln!("native Task borrow contract failed: expected compiler executable argument");
        std::process::exit(2);
    };
    if args.next().is_some() {
        eprintln!("native Task borrow contract failed: unexpected extra arguments");
        std::process::exit(2);
    }

    let compiler = PathBuf::from(compiler);
    let temp = match TempDir::new() {
        Ok(temp) => temp,
        Err(err) => {
            eprintln!("native Task borrow contract failed: {err}");
            std::process::exit(1);
        }
    };

    for case in CASES {
        if let Err(err) = run_case(&compiler, temp.path(), *case) {
            eprintln!("native Task borrow contract failed: {err}");
            std::process::exit(1);
        }
    }

    println!(
        "PASS: {} borrowed Task forms rejected before LLVM IR emission",
        CASES.len()
    );
}
