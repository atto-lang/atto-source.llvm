use std::{env, fs, path::PathBuf, process};

fn fail(message: &str) -> ! {
    eprintln!("DENIED: {message}");
    process::exit(1);
}

fn call_slice<'a>(text: &'a str, callee: &str) -> Option<&'a str> {
    let start = text.find(callee)?;
    let rest = &text[start..];
    let end = rest.find(");")?;
    Some(&rest[..end + 2])
}

fn require_call_token(text: &str, callee: &str, tokens: &[&str], label: &str) {
    let call = call_slice(text, callee)
        .unwrap_or_else(|| fail(&format!("native nonblocking I/O invariant missing: {label}")));
    for token in tokens {
        if !call.contains(token) {
            fail(&format!("native nonblocking I/O invariant missing: {label}"));
        }
    }
}

fn main() {
    let root = env::args().nth(1).map(PathBuf::from)
        .unwrap_or_else(|| fail("repository root argument required"));
    let path = root.join("src/main/cpp/oreslang/runtime/NonblockingTcpLinux.hpp");
    let text = fs::read_to_string(&path)
        .unwrap_or_else(|e| fail(&format!("cannot read {}: {e}", path.display())));

    require_call_token(&text, "::socket(", &["SOCK_STREAM", "SOCK_NONBLOCK", "SOCK_CLOEXEC"],
        "socket_nonblock");
    require_call_token(&text, "::recv(", &["MSG_DONTWAIT"], "recv_dontwait");
    require_call_token(&text, "::send(", &["MSG_DONTWAIT", "MSG_NOSIGNAL"], "send_dontwait");
    require_call_token(&text, "::accept4(", &["SOCK_NONBLOCK", "SOCK_CLOEXEC"], "accept4_nonblock");
    require_call_token(&text, "::eventfd(", &["EFD_NONBLOCK", "EFD_CLOEXEC"], "eventfd_nonblock");

    if !text.contains("EPOLLONESHOT") {
        fail("native nonblocking I/O invariant missing: one_shot_readiness");
    }
    if text.contains("::accept(") {
        fail("blocking socket primitive introduced: blocking_accept");
    }

    for (callee, label) in [("::recv(", "blocking_recv_flags_zero"),
                            ("::send(", "blocking_send_flags_zero")] {
        let call = call_slice(&text, callee)
            .unwrap_or_else(|| fail(&format!("native nonblocking I/O invariant missing: {label}")));
        let compact: String = call.chars().filter(|c| !c.is_whitespace()).collect();
        if compact.ends_with(",0);") {
            fail(&format!("blocking socket primitive introduced: {label}"));
        }
    }

    println!("PASS: Linux TCP substrate remains nonblocking/readiness-driven");
}
