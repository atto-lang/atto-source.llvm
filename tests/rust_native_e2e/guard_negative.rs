use super::Harness;

// Unsafe language shapes must fail before any IR, object or executable is emitted.
pub fn run(h: &mut Harness) {
    h.negative("caret_missing_value",
        "pub fnc main(): int { ^; }\n", &[], None);
    h.negative("caret_is_not_expression",
        "pub fnc main(): int { return ^ 42; }\n", &[], None);
    h.negative("boolean_xor_is_not_return",
        "pub fnc main(): int { ^^ 42; }\n", &[], None);
    h.negative("guard_rejects_int_truthiness",
        "pub fnc main(): int { guard 1 { ^ 7; } ^ 42; }\n",
        &[], Some("bool"));
    h.negative("guard_rejects_double_exit",
        "pub fnc main(): int { guard false { ^ 7; ^ 8; } ^ 42; }\n",
        &[], None);
    h.negative("mutex_bare_class_constructor",
        "pub fnc main(): int { return Mutex<int>(0); }\n",
        &[], Some("E_NEW_REQUIRED"));
    h.negative("mutex_new_runtime_unimplemented",
        "pub fnc main(): int { return new Mutex<int>(0); }\n",
        &[], Some("E_LOCK_UNSUPPORTED"));
    h.negative("mutex_nested_generic_without_new",
        "pub fnc main(): int { return Mutex<Result<int,string>>(0); }\n",
        &[], Some("E_NEW_REQUIRED"));
    h.negative("suspending_native_await_rejected",
        "pub suspends fnc main(): int { let Task<int> pending = task { ^ 41; }; val answer: int = await pending; ^ answer; }\n",
        &[], Some("suspension-aware borrow checking"));
}
