use super::Harness;

// Every invalid source must reject before LLVM IR and leave only input.atto
// in the isolated compiler directory. Host Rust tests, no Python harness.
fn local(body: &str) -> String {
    format!("pub fnc main(): int {{ let value: int = 42; {body} }}")
}

pub fn run(h: &mut Harness) {
    let flags = ["--experimental-pointer-int-aot"];
    let placement = "define struct Point { int x; int y; } with { constructor(int x, int y) { self.x = x; self.y = y; } } pub fnc main(): int { let Storage<Point> s = alloc! Point; let mut RawPointer<Point> p = RawPointer.empty<Point>(); ";
    for (name,body) in [
        ("read_empty", "val first = (deref! p).x; free! s; return first;"),
        ("bind_empty", "ref! p to s; free! s; return p;"),
        ("double_construct", "new Point(40, 2) at s; new Point(1, 2) at s; destroy! s; free! s; return p;"),
        ("destroy_bound", "new Point(40, 2) at s; ref! p to s; destroy! s; free! s; return p;"),
        ("free_live", "new Point(40, 2) at s; free! s; return p;"),
        ("free_twice", "free! s; free! s; return p;"),
        ("field_escape", "new Point(40, 2) at s; ref! p to s; val first = (deref! p).z; unref! p; destroy! s; free! s; return first;"),
        ("leak", "new Point(40, 2) at s; ref! p to s; val first = (deref! p).x; unref! p; destroy! s; return first;"),
        ("null_clear", "new Point(40, 2) at s; ref! p to s; p = null; destroy! s; free! s; return p;"),
    ] {
        h.negative(&format!("placement_{name}"), &format!("{placement}{body} }}"), &flags, None);
    }
    for (name, body) in [
        ("empty_read", "val x = deref! p; return x;"),
        ("null", "p = null; return 42;"),
        ("double_detach", "ref! p to a; unref! p; unref! p; return 42;"),
        ("live_rebind", "ref! p to a; ref! p to a; unref! p; return 42;"),
        ("live_exit", "ref! p to a; return 42;"),
        ("escape", "return p;"),
    ] {
        let source = format!("pub fnc main(): int {{ let a: int = 42; let mut RawPointer<int> p = RawPointer.empty<int>(); {body} }}");
        h.negative(&format!("slot_{name}"), &source, &flags, None);
    }
    let invalid = [
        ("missing_unref", "val Pointer<int> p = rt ref value; val answer = rt deref p; return answer;", "unref"),
        ("double_unref", "val Pointer<int> p = rt ref value; val answer = rt deref p; rt unref p; unref!p; return answer;", "expected"),
        ("wrong_deref", "val Pointer<int> p = rt ref value; val answer = rt deref value; rt unref p; return answer;", "expected"),
        ("wrong_release", "val Pointer<int> p = rt ref value; val answer = rt deref p; rt unref value; return answer;", "expected"),
        ("scalar_cast", "val Pointer<int> p = rt ref value; val answer = p as int; rt unref p; return answer;", "expected"),
        ("copied_handle", "val Pointer<int> p = rt ref value; val Pointer<int> clone = p; val answer = rt deref p; rt unref p; return answer;", "duplicate or reserved immutable local"),
        ("mutated_referent", "val Pointer<int> p = rt ref value; value = 43; val answer = rt deref p; rt unref p; return answer;", "expected"),
        ("unsupported_close", "val Pointer<int> p = rt ref value; val answer = rt deref p; p.close(); return answer;", "expected"),
        ("unsupported_write", "val Pointer<int> p = rt ref value; p.write_at(0, 99); val answer = rt deref p; rt unref p; return answer;", "expected"),
        ("bad_ref_spacing", "val Pointer<int> p = ref !value; val answer = rt deref p; rt unref p; return answer;", "adjacent"),
        ("bad_deref_spacing", "val Pointer<int> p = rt ref value; val answer = deref !p; rt unref p; return answer;", "adjacent"),
        ("bad_unref_spacing", "val Pointer<int> p = rt ref value; val answer = deref! p; unref !p; return answer;", "adjacent"),
        ("escaped_pointer", "val Pointer<int> p = rt ref value; val answer = rt deref p; rt unref p; return p;", "expected"),
        ("unauthorized_call", "val Pointer<int> p = rt ref value; val answer = rt deref p; fnc foo(); rt unref p; return answer;", "expected"),
        ("branch_cleanup", "val Pointer<int> p = rt ref value; val answer = rt deref p; if true then rt unref p; fi return answer;", "expected"),
        ("shadowed_handle", "val Pointer<int> value = rt ref value; val answer = deref! value; unref! value; return answer;", "shadowing"),
        ("wrong_type", "val Pointer<bool> p = rt ref value; val answer = deref! p; unref! p; return answer;", "expected"),
        ("forged_address", "val Pointer<int> p = &value; val answer = deref! p; unref! p; return answer;", "unsupported oreslang token"),
    ];
    for (name, body, diagnostic) in invalid {
        h.negative(&format!("pointer_local_{name}"), &local(body), &flags, Some(diagnostic));
    }
    h.negative("pointer_local_uninitialized",
        "pub fnc main(): int { let value: int; val Pointer<int> p = rt ref value; val answer = deref! p; unref! p; return answer; }",
        &flags, Some("expected"));
    h.negative("pointer_local_without_opt_in",
        &local("val Pointer<int> p = rt ref value; val answer = rt deref p; rt unref p; return answer;"),
        &[], Some("unsupported"));
    h.negative("pointer_local_extra_function",
        &format!("{} pub fnc extra(): int {{ return 0; }}",
            local("val Pointer<int> p = rt ref value; val answer = rt deref p; rt unref p; return answer;")),
        &flags, Some("trailing"));
}
