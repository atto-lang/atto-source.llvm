// Generated from stdlib/math.atto; do not edit manually.
// source Git blob SHA-1: 3c1ca889b795e294aba6349c538e782461335e90
R"ATTO_MATH(// LLVM backend-owned freestanding Atto core math; guest source only.
// No host Math calls, heap, OS or unsafe signed abs(MIN) conversion.
define module math as
  pub fnc min_int(int left, int right): int {
    if left < right; do return left; fi
    return right;
  }
  pub fnc max_int(int left, int right): int {
    if left > right; do return left; fi
    return right;
  }
  pub fnc signum_int(int value): int {
    if value < 0; do return -1; fi
    if value > 0; do return 1; fi
    return 0;
  }
  pub fnc contains_closed_int(int value, int lower, int upper): bool {
    if lower > upper; do return false; fi
    return value >= lower && value <= upper;
  }
  // A fallible clamp needs typed Result lowering; never silently clamp
  // invalid bounds or coerce signed minimum to an unrepresentable abs.
end
)ATTO_MATH"
