//! Exhaust the alias partitions and loan modes of two/three argument calls.
//! This is a bounded invocation contract, not a proof of the entire language.
use std::{
    collections::HashSet,
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

fn partitions(n: usize) -> Vec<Vec<usize>> {
    fn extend(n: usize, prefix: &mut Vec<usize>, out: &mut Vec<Vec<usize>>) {
        if prefix.len() == n {
            out.push(prefix.clone());
            return;
        }
        let last = prefix.iter().copied().max().map_or(0, |v| v + 1);
        for owner in 0..=last {
            prefix.push(owner);
            extend(n, prefix, out);
            prefix.pop();
        }
    }
    let mut result = Vec::new();
    extend(n, &mut vec![0], &mut result);
    result
}

fn conflicts(owners: &[usize], modes: usize) -> bool {
    (0..owners.len()).any(|i| {
        ((i + 1)..owners.len())
            .any(|j| owners[i] == owners[j] && ((modes >> i) & 1 != 0 || (modes >> j) & 1 != 0))
    })
}

fn source(kind: &str, owners: &[usize], modes: usize) -> String {
    let n = owners.len();
    let count = owners.iter().max().unwrap() + 1;
    let unit = 42 / n;
    let type_name = if kind == "class" { "Cell" } else { kind };
    let prefix = if kind == "class" {
        format!("pub define class Cell as pub let int value={unit}; end\n")
    } else {
        String::new()
    };
    let parameters = (0..n)
        .map(|i| {
            format!(
                "borrow {}{type_name} p{i}",
                if (modes >> i) & 1 != 0 { "mut " } else { "" }
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    let reads = (0..n)
        .map(|i| {
            if kind == "class" {
                format!("p{i}.value")
            } else {
                format!("(rt copy p{i})")
            }
        })
        .collect::<Vec<_>>();
    let result = if kind == "bool" {
        format!("({}) ? 42 : 0", reads.join(" && "))
    } else {
        reads.join(" + ")
    };
    let declarations = (0..count)
        .map(|i| match kind {
            "class" => format!("let mut Cell v{i}=new Cell();"),
            "bool" => format!("val v{i}:bool=true;"),
            _ => format!("val v{i}:int={unit};"),
        })
        .collect::<String>();
    let arguments = owners
        .iter()
        .enumerate()
        .map(|(i, owner)| {
            format!(
                "rt {}borrow v{owner}",
                if (modes >> i) & 1 != 0 { "mut " } else { "" }
            )
        })
        .collect::<Vec<_>>()
        .join(",");
    // Calling twice catches leaked argument loans. The conditional call also
    // checks that the common caller state survives a lazy expression boundary.
    format!(
        "{prefix}fnc inspect({parameters}):int{{return {result};}}\n\
        pub fnc main():int{{{declarations}const first:int=inspect({arguments});\
        return first == 42 ? inspect({arguments}) : 0;}}\n"
    )
}

const SHARED_CORPUS_CASES: usize = 166;

fn main() {
    let mut compiler = None;
    let mut clang = None;
    let mut java = None;
    let mut sanitize = false;
    let mut corpus = None;
    let mut input_corpus = None;
    let mut args = env::args().skip(1);
    while let Some(arg) = args.next() {
        match arg.as_str() {
            "--compiler" => compiler = args.next(),
            "--clang" => clang = args.next(),
            "--java-command" => java = Some(args.next().expect("--java-command PATH required")),
            "--sanitize" => sanitize = true,
            "--emit-corpus" => corpus = Some(args.next().expect("--emit-corpus PATH required")),
            "--corpus" => input_corpus = Some(args.next().expect("--corpus PATH required")),
            _ => panic!("unknown argument: {arg}"),
        }
    }
    if let Some(destination) = corpus {
        let mut lines = String::new();
        for kind in ["int", "bool", "class"] {
            for n in [2, 3] {
                for modes in 0..(1 << n) {
                    for owners in partitions(n) {
                        let name = format!("matrix-{kind}-{n}-{modes}-{owners:?}");
                        let verdict = if conflicts(&owners, modes) {
                            "reject"
                        } else {
                            "accept"
                        };
                        lines.push_str(&format!(
                            "{verdict}\t{name}\t{}\n",
                            source(kind, &owners, modes).replace('\n', " ").trim()
                        ));
                    }
                }
            }
        }
        assert_eq!(lines.lines().count(), 144);
        fs::write(destination, lines).unwrap();
        return;
    }
    let compiler = compiler.expect("--compiler PATH required");
    let clang = clang.expect("--clang PATH required");
    let suffix = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let scratch =
        Scratch(env::temp_dir().join(format!("ores-loans-{}-{suffix}", std::process::id())));
    fs::create_dir(&scratch.0).unwrap();
    let (mut accepted, mut rejected, mut compared) = (0, 0, 0);
    let from_corpus = input_corpus.is_some();
    let mut cases = Vec::new();
    if let Some(path) = input_corpus {
        let contents = fs::read_to_string(path).expect("cannot read borrow corpus");
        let mut names = HashSet::new();
        for line in contents.lines() {
            let fields: Vec<_> = line.splitn(3, '\t').collect();
            assert_eq!(fields.len(), 3, "malformed corpus row");
            assert!(matches!(fields[0], "accept" | "reject"), "unknown verdict");
            assert!(
                !fields[1].is_empty() && names.insert(fields[1].to_owned()),
                "duplicate/empty case name"
            );
            assert!(!fields[2].trim().is_empty(), "empty case source");
            cases.push((
                fields[1].to_owned(),
                fields[2].to_owned(),
                fields[0] == "reject",
            ));
        }
        assert_eq!(
            cases.len(),
            SHARED_CORPUS_CASES,
            "shared corpus must contain all 162 generated vectors and 4 heap projection regressions"
        );
    } else {
        for kind in ["int", "bool", "class"] {
            for n in [2, 3] {
                let layouts = partitions(n);
                assert_eq!(layouts.len(), if n == 2 { 2 } else { 5 });
                for modes in 0..(1 << n) {
                    for owners in &layouts {
                        cases.push((
                            format!("matrix-{kind}-{n}-{modes}-{owners:?}"),
                            source(kind, owners, modes),
                            conflicts(owners, modes),
                        ));
                    }
                }
            }
        }
    }
    for (name, code, invalid) in cases {
        let path = scratch.0.join("case.ores");
        fs::write(&path, code).unwrap();
        let native = run(Command::new(&compiler).arg(&path));
        let diagnostic = String::from_utf8_lossy(&native.stderr);
        if invalid {
            assert_eq!(native.status.code(), Some(1), "{name}: {diagnostic}");
            assert!(
                native.stdout.is_empty(),
                "{name}: invalid loan emitted LLVM IR"
            );
            assert!(
                diagnostic.to_lowercase().contains("borrow")
                    && !diagnostic.to_lowercase().contains("internal")
                    && (!name.starts_with("matrix-")
                        || diagnostic.contains("conflicting active native borrow")),
                "{name}: {diagnostic}"
            );
            rejected += 1;
        } else {
            assert!(
                native.status.success() && !native.stdout.is_empty(),
                "{name}: {diagnostic}"
            );
            let ir = scratch.0.join("case.ll");
            let binary = scratch.0.join("case-native");
            fs::write(&ir, &native.stdout).unwrap();
            if binary.exists() {
                fs::remove_file(&binary).unwrap();
            }
            let mut link = Command::new(&clang);
            link.arg("-O1");
            if sanitize {
                link.args([
                    "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer",
                ]);
            }
            let linked = run(link.arg(&ir).arg("-o").arg(&binary));
            assert!(
                linked.status.success(),
                "{name}: {}",
                String::from_utf8_lossy(&linked.stderr)
            );
            assert!(
                binary.is_file(),
                "{name}: linker reported success without an executable"
            );
            let executed = run(&mut Command::new(&binary));
            assert_eq!(
                executed.status.code(),
                Some(42),
                "{name}: {}",
                String::from_utf8_lossy(&executed.stderr)
            );
            accepted += 1;
        }
        if let Some(launcher) = &java {
            let mut command = Command::new(launcher);
            if invalid {
                command.arg("--check");
            }
            let reference = run(command.arg(&path));
            let message = String::from_utf8_lossy(&reference.stderr);
            if invalid {
                assert_eq!(reference.status.code(), Some(1), "Java {name}: {message}");
                assert!(
                    (!name.starts_with("matrix-")
                        && message.to_lowercase().contains("borrow")
                        && !message.to_lowercase().contains("internal"))
                        || message.contains("mutably borrowed")
                        || message.contains("another borrow is active")
                        || message.contains("overlapping mutable borrow arguments"),
                    "Java {name}: wrong rejection: {message}"
                );
            } else {
                assert_eq!(reference.status.code(), Some(42), "Java {name}: {message}");
            }
            compared += 1;
        }
    }
    if from_corpus {
        assert_eq!(accepted + rejected, SHARED_CORPUS_CASES);
    } else {
        assert_eq!((accepted, rejected), (60, 84));
    }
    println!("PASS borrow contract: accepted={accepted}, rejected={rejected}, Java={compared}, sanitized={sanitize}");
}
