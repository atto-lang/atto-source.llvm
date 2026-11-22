use super::Harness;

// Outcomes are OS exit codes of Clang-linked executables from checked Atto source.
pub fn run(h: &mut Harness) {
    h.positive("caret_return_42", "pub fnc main(): int { ^ 42; }\n", &[], 42, false, None);
    h.positive("guard_false_7",
        "pub fnc main(): int { guard false { ^ 7; } ^ 42; }\n",
        &[], 7, false, None);
    h.positive("guard_true_42",
        "pub fnc main(): int { guard true { ^ 7; } ^ 42; }\n",
        &[], 42, false, None);
    h.positive("caret_helper_7",
        "fnc pick(bool ready): int { guard ready { ^ 7; } ^ 42; }\npub fnc main(): int { ^ pick(false); }\n",
        &[], 7, false, None);
    h.positive("legacy_return_42", "pub fnc main(): int { return 6 * 7; }\n",
        &[], 42, false, None);
}
