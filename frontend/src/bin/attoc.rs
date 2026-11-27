//! Source-local frontend compiler CLI: attoc input.atto [-o out.wasm].
//! Outputs Wasm bytes from the existing bounded compiler, NOT LLVM native AOT.
use std::io::Write;
use std::process::ExitCode;

fn main() -> ExitCode {
    let mut args: Vec<String> = std::env::args().skip(1).collect();
    let native_c = args.first().is_some_and(|s| s == "--emit=struct-native-c");
    if native_c { args.remove(0); }
    let (input, output) = match args.as_slice() {
        [input] => (input.as_str(), None),
        [input, flag, output] if flag == "-o" => (input.as_str(), Some(output.as_str())),
        _ => {
            eprintln!("usage: attoc [--emit=struct-native-c] <input.atto> [-o output]");
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
    let compiled = if native_c { atto_compiler::compile_struct_native_c(&source).map(String::into_bytes) } else { atto_compiler::compile(&source) };
    let bytes = match compiled {
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
            if file.write_all(&bytes).is_err() || file.sync_all().is_err() {
                drop(file);
                let _ = std::fs::remove_file(path);
                eprintln!("{path}: output artifact write failed");
                return ExitCode::from(2);
            }
        }
        None => {
            if std::io::stdout().write_all(&bytes).is_err() {
                eprintln!("cannot write compiler output");
                return ExitCode::from(2);
            }
        }
    }
    ExitCode::SUCCESS
}
