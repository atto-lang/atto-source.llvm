//! Minimal frontend CLI for exact-source conformance and guest Wasm tests.
//! This is the Rust/Wasm frontend in the LLVM repository, NOT the native C++ AOT compiler.
use std::{env, fs, io::{self, Write}, process::ExitCode};

fn main() -> ExitCode {
    let args: Vec<String> = env::args().skip(1).collect();
    let (input, output) = match args.as_slice() {
        [input] if !input.starts_with('-') => (input.as_str(), None),
        [input, flag, output] if !input.starts_with('-') && flag == "-o" =>
            (input.as_str(), Some(output.as_str())),
        _ => {
            eprintln!("usage: attoc input.atto [-o guest.wasm]");
            return ExitCode::from(2);
        }
    };
    let source = match fs::read_to_string(input) {
        Ok(source) => source,
        Err(error) => {
            eprintln!("E_IO: cannot read source: {error}");
            return ExitCode::from(2);
        }
    };
    let guest = match atto_compiler::compile(&source) {
        Ok(guest) => guest,
        Err(error) => {
            eprintln!("{}: {}", input, error);
            return ExitCode::from(1);
        }
    };
    let result = match output {
        Some(path) => fs::write(path, &guest),
        None => io::stdout().write_all(&guest),
    };
    if let Err(error) = result {
        eprintln!("E_IO: cannot write guest artifact: {error}");
        return ExitCode::from(2);
    }
    ExitCode::SUCCESS
}
