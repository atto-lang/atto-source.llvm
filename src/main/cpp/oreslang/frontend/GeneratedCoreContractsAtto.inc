R"ATTO_CORE4(// LLVM-owned Atto source; independent of other compiler repository implementations.
// Total predicates and explicit trappable contract failures.
// Calls must be retained in optimized/JIT/AOT modes: these are not debug asserts.
// Condition is a bool evaluated once by the ordinary argument-evaluation rules.
define module contracts as
  pub fnc check(bool condition): bool {
    return condition;
  }
  pub fnc check_message(bool condition, String message): bool {
    // Message is intentionally ignored on success; check has no side effects.
    return condition;
  }
  pub fnc requires(bool condition, String message): void {
    if !condition; do
      rt fail(message);
    fi
    return;
  }
  pub fnc ensures(bool condition, String message): void {
    if !condition; do
      rt fail(message);
    fi
    return;
  }
  pub fnc invariant(bool condition, String message): void {
    if !condition; do
      rt fail(message);
    fi
    return;
  }
  pub fnc unreachable(String message): void {
    rt fail(message);
    return;
  }
end
)ATTO_CORE4"
