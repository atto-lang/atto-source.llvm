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

    let string_path = root.join("src/main/cpp/oreslang/runtime/NativeStringRegion.c");
    let string_text = fs::read_to_string(&string_path)
        .unwrap_or_else(|e| fail(&format!("cannot read {}: {e}", string_path.display())));

    for token in [
        "OUTPUT_SLOTS = 1024",
        "pthread_create(&r->writer, NULL, output_reactor, r)",
        "r->original_flags | O_NONBLOCK",
        "flags | O_NONBLOCK",
        "S_ISFIFO(status.st_mode)",
        "S_ISSOCK(status.st_mode)",
        "isatty(STDOUT_FILENO)",
    ] {
        if !string_text.contains(token) {
            fail(&format!("native stdout async invariant missing: {token}"));
        }
    }

    let print_start = string_text.find("int64_t ores_string_print(")
        .unwrap_or_else(|| fail("native stdout publish function missing"));
    let print_rest = &string_text[print_start..];
    // End at the function's own closing brace: main places the panic-time
    // flush (which legitimately joins the writer) between print and end.
    let print_end = print_rest.find("\n}\n")
        .unwrap_or_else(|| fail("cannot delimit native stdout publish function"));
    let print_body = &print_rest[..print_end];
    for forbidden in ["write(", "poll(", "pthread_join(", "fwrite(", "printf(", "puts("] {
        if print_body.contains(forbidden) {
            fail(&format!("guest stdout publish path may block: {forbidden}"));
        }
    }
    if !print_body.contains("head - atomic_load_explicit(&r->tail")
            || !print_body.contains("== OUTPUT_SLOTS")
            || !print_body.contains("wake_writer(r)") {
        fail("guest stdout publish must use bounded queue and nonblocking wake");
    }

    let reactor_start = string_text.find("static void *output_reactor(")
        .unwrap_or_else(|| fail("native stdout reactor missing"));
    let reactor_end = string_text[reactor_start..].find("\n}\nstatic void wake_writer")
        .unwrap_or_else(|| fail("cannot delimit native stdout reactor"));
    let reactor_body = &string_text[reactor_start..reactor_start + reactor_end];
    if !reactor_body.contains("write_ready(") || !reactor_body.contains("poll(") {
        fail("native stdout writes must remain on dedicated readiness reactor");
    }

    let end_start = string_text.find("int64_t ores_string_region_end(")
        .unwrap_or_else(|| fail("native stdout retirement function missing"));
    let end_body = &string_text[end_start..];
    if !end_body.contains("pthread_join(r->writer, NULL)") {
        fail("native stdout physical retirement must join the writer after guest execution");
    }

    println!("PASS: native TCP and stdout substrates remain nonblocking/readiness-driven");
}
