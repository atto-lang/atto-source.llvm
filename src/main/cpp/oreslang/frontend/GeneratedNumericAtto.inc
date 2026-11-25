// Generated from stdlib/numeric.atto; do not edit manually.
// source Git blob SHA-1: 5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72
R"ATTO_NUMERIC(// Compiler-owned ores.core source.
// This module has no OS, Java, filesystem, network, process, actor-scheduler,
// or ambient project dependency.

define module numeric as

  pub fnc min_int(int left, int right): int {
    if left < right; do
      return left;
    fi
    return right;
  }

  pub fnc max_int(int left, int right): int {
    if left > right; do
      return left;
    fi
    return right;
  }

  pub fnc clamp_int(int value, int lower, int upper): int {
    if lower > upper; do
      return lower;
    fi
    if value < lower; do
      return lower;
    fi
    if value > upper; do
      return upper;
    fi
    return value;
  }

end
)ATTO_NUMERIC"
