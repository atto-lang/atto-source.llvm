use std::{
    env, fs, io,
    path::Path,
    process::{self, Command},
};

fn forbidden_path(path: &str) -> bool {
    let p = path.to_ascii_lowercase();
    let file = p.rsplit('/').next().unwrap_or(&p);
    p.ends_with(".py")
        || p.ends_with(".pyi")
        || p.ends_with(".pyw")
        || p.ends_with(".ipynb")
        || file == "pipfile"
        || file == "pipfile.lock"
        || file == "poetry.lock"
        || file == "pyproject.toml"
        || file == "uv.lock"
        || file == "setup.py"
        || file == "tox.ini"
        || file == ".python-version"
        || file == "pdm.lock"
        || (file.starts_with("requirements") && file.ends_with(".txt"))
}

fn is_execution_surface(path: &str) -> bool {
    let p = path.to_ascii_lowercase();
    let file = p.rsplit('/').next().unwrap_or(&p);
    (p.starts_with(".github/workflows/") && (p.ends_with(".yaml") || p.ends_with(".yml")))
        || (p.starts_with(".github/actions/") && (p.ends_with(".yaml") || p.ends_with(".yml")))
        || file == "makefile"
        || file == "justfile"
        || file == "cmakelists.txt"
        || file == "dockerfile"
        || p.ends_with(".cmake")
        || p.ends_with(".mk")
        || p.ends_with(".sh")
        || p.ends_with(".bash")
        || p.ends_with(".zsh")
}

fn forbidden_execution_line(line: &str) -> bool {
    let l = line.trim().to_ascii_lowercase();
    if l.starts_with('#') {
        return false;
    }
    if l.contains("find_package(python")
        || l.contains("python3_add_")
        || l.contains("python_add_")
        || l.contains("python3_executable")
    {
        return true;
    }
    [
        "actions/setup-python@",
        "python -m ",
        "python3 -m ",
        "python -c ",
        "python3 -c ",
        "python ",
        "python3 ",
        "pypy ",
        "pypy3 ",
        "pip install",
        "pip3 install",
        "uv run ",
        "pytest ",
        "poetry run ",
        "pdm run ",
    ]
    .iter()
    .any(|needle| l.contains(needle))
}

fn python_shebang(body: &str) -> bool {
    let first = body
        .lines()
        .next()
        .unwrap_or("")
        .trim()
        .to_ascii_lowercase();
    first.starts_with("#!")
        && (first.contains("/python")
            || first.contains("env python")
            || first.contains("env -s python")
            || first.contains("/pypy")
            || first.contains("env pypy"))
}

fn violations(path: &str, body: &str) -> Vec<String> {
    let mut messages = Vec::new();
    if forbidden_path(path) {
        messages
            .push("Python source, notebook, dependency manifest, or environment file".to_string());
    }
    if python_shebang(body) {
        messages.push("Python/PyPy shebang".to_string());
    }
    if is_execution_surface(path) {
        for (index, line) in body.lines().enumerate() {
            if forbidden_execution_line(line) {
                messages.push(format!("Python tooling invocation at line {}", index + 1));
            }
        }
    }
    messages
}

// Only genuinely binary non-execution assets may skip content inspection.
// A tracked path that cannot be read, or an execution surface with invalid
// UTF-8, must never turn into an empty string and appear to pass the gate.
fn read_policy_text(path: &Path, execution_surface: bool) -> io::Result<Option<String>> {
    let bytes = fs::read(path)?;
    match String::from_utf8(bytes) {
        Ok(body) => Ok(Some(body)),
        Err(error) => {
            if execution_surface || error.as_bytes().starts_with(b"#!") {
                Err(io::Error::new(
                    io::ErrorKind::InvalidData,
                    "non-UTF-8 executable surface or shebang",
                ))
            } else {
                Ok(None)
            }
        }
    }
}

fn main() {
    let root = env::args().nth(1).unwrap_or_else(|| ".".to_string());
    let output = Command::new("git")
        .args(["-C", &root, "ls-files", "-z"])
        .output()
        .expect("git must be installed");
    if !output.status.success() {
        eprintln!("git ls-files failed");
        process::exit(2);
    }

    let mut found = 0usize;
    for raw in output
        .stdout
        .split(|byte| *byte == 0)
        .filter(|v| !v.is_empty())
    {
        let name = match std::str::from_utf8(raw) {
            Ok(name) => name,
            Err(_) => {
                eprintln!("FAIL-CLOSED: git listed a non-UTF-8 tracked path");
                process::exit(2);
            }
        };
        let file = Path::new(&root).join(name);

        let body = match read_policy_text(&file, is_execution_surface(name)) {
            Ok(Some(body)) => body,
            Ok(None) => String::new(),
            Err(error) => {
                eprintln!("FAIL-CLOSED: cannot inspect tracked file {name}: {error}");
                process::exit(2);
            }
        };
        for problem in violations(name, &body) {
            eprintln!("{name}: {problem}");
            found += 1;
        }
    }

    if found > 0 {
        eprintln!("{found} Python policy violation(s)");
        process::exit(1);
    }
    println!("Rust-only tooling policy check passed");
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn blocks_python_assets_and_manifests() {
        for path in [
            "tool/doit.py",
            "tool/doit.pyw",
            "types/x.pyi",
            "tests/a.ipynb",
            "pyproject.toml",
            "requirements-dev.txt",
            "Pipfile.lock",
            ".python-version",
            "tox.ini",
        ] {
            assert!(forbidden_path(path), "must reject {path}");
        }
        assert!(!forbidden_path("README.md"));
        assert!(!forbidden_path("src/main.rs"));
    }

    #[test]
    fn recognizes_execution_surfaces() {
        assert!(is_execution_surface(".github/workflows/ci.yml"));
        assert!(is_execution_surface(".github/actions/check/action.yaml"));
        assert!(is_execution_surface("scripts/check.sh"));
        assert!(is_execution_surface("Makefile"));
        assert!(is_execution_surface("justfile"));
        assert!(is_execution_surface("CMakeLists.txt"));
        assert!(is_execution_surface("tests/guest.cmake"));
        assert!(is_execution_surface("Dockerfile"));
        assert!(is_execution_surface("lib/build.mk"));
        assert!(!is_execution_surface("docs/policy.md"));
    }

    #[test]
    fn blocks_python_ci_invocations() {
        assert!(forbidden_execution_line("- uses: actions/setup-python@v5"));
        assert!(forbidden_execution_line("- run: python3 -m pytest"));
        assert!(forbidden_execution_line("pip install -r requirements.txt"));
        assert!(forbidden_execution_line("uv run tool.py"));
        assert!(forbidden_execution_line("find_package(Python3 COMPONENTS Interpreter)"));
        assert!(forbidden_execution_line("python3_add_library(native MODULE code.c)"));
        assert!(forbidden_execution_line("set(Python3_EXECUTABLE /usr/bin/python3)"));
        assert!(!forbidden_execution_line("- run: cargo test"));
    }

    #[test]
    fn blocks_extensionless_python_scripts() {
        assert!(python_shebang("#!/usr/bin/env python3\nprint('x')"));
        assert!(python_shebang("#!/usr/bin/python\nprint('x')"));
        assert!(python_shebang("#!/usr/bin/env pypy3\nprint('x')"));
        assert!(!python_shebang("#!/usr/bin/env bash\necho ok"));
    }

    #[test]
    fn non_utf8_execution_surfaces_fail_closed() {
        let root = std::env::temp_dir().join(format!(
            "oreslang-no-python-preflight-{}",
            std::process::id()
        ));
        let _ = fs::remove_dir_all(&root);
        fs::create_dir_all(&root).unwrap();

        let workflow = root.join("workflow.yml");
        fs::write(&workflow, b"run: cargo test\n\xff").unwrap();
        assert_eq!(
            read_policy_text(&workflow, true).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );

        let shebang = root.join("entry");
        fs::write(&shebang, b"#!/usr/bin/env python3\n\xff").unwrap();
        assert_eq!(
            read_policy_text(&shebang, false).unwrap_err().kind(),
            io::ErrorKind::InvalidData
        );

        let binary = root.join("payload.bin");
        fs::write(&binary, [0xff, 0xfe]).unwrap();
        assert_eq!(read_policy_text(&binary, false).unwrap(), None);
        assert!(read_policy_text(&root.join("missing"), false).is_err());

        let allowed = root.join("check.sh");
        fs::write(&allowed, b"#!/bin/sh\ncargo test\n").unwrap();
        assert_eq!(
            read_policy_text(&allowed, true).unwrap().as_deref(),
            Some("#!/bin/sh\ncargo test\n")
        );
        fs::remove_dir_all(&root).unwrap();
    }

    #[test]
    fn prose_is_not_treated_as_execution() {
        let body = "Python is prohibited; replace Python tooling with Rust.";
        assert!(violations("README.md", body).is_empty());
    }
}
