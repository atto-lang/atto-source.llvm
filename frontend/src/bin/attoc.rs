//! Source-local frontend compiler CLI: attoc input.atto [-o out.wasm].
//! Outputs Wasm bytes from the existing bounded compiler, NOT LLVM native AOT.
use std::io::Write;
use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let (input, output) = match args.as_slice() {
        [input] => (input.as_str(), None),
        [input, flag, output] if flag == "-o" => (input.as_str(), Some(output.as_str())),
        _ => {
            eprintln!("usage: attoc <input.atto> [-o out.wasm]");
            return ExitCode::from(2);
        }
    };
    let source = match std::fs::read(input).and_then(|bytes| {
        String::from_utf8(bytes).map_err(|error| {
            std::io::Error::new(std::io::ErrorKind::InvalidData, error)
        })
    }) {
        Ok(source) => source,
        Err(_) => {
            eprintln!("{input}: cannot read valid UTF-8 source");
            return ExitCode::from(2);
        }
    };
    let wasm = match atto_compiler::compile(&source) {
        Ok(bytes) => bytes,
        Err(error) => {
            eprintln!("{input}:{error}");
            return ExitCode::from(1);
        }
    };
    match output {
        Some(path) => {
            // Never truncate an existing artifact. An unverified partial
            // output must not be mistaken for a successful compiler run.
            let mut file = match std::fs::OpenOptions::new()
                .write(true).create_new(true).open(path) {
                Ok(file) => file,
                Err(_) => {
                    eprintln!("{path}: cannot create new output artifact");
                    return ExitCode::from(2);
                }
            };
            if file.write_all(&wasm).is_err() || file.sync_all().is_err() {
                drop(file);
                let _ = std::fs::remove_file(path);
                eprintln!("{path}: output artifact write failed");
                return ExitCode::from(2);
            }
        }
        None => {
            if std::io::stdout().write_all(&wasm).is_err() {
                eprintln!("cannot write compiler output");
                return ExitCode::from(2);
            }
        }
    }
    ExitCode::SUCCESS
}
