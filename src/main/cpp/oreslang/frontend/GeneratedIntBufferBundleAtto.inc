// Generated from stdlib/alloc/int_buffer_bundle.atto; do not edit manually.
// source Git blob SHA-1: 07c7083e099a18f1db7097f9639efc677070eb95
R"ATTO_INT_BUNDLE(// Compiler-owned alloc tier transitive-closure probe.
// The local body is allocation-free, but linking this module brings in the
// allocator-bearing int_buffer dependency and therefore requires Allocator.

import module int_buffer from "atto.alloc/int_buffer";

define module int_buffer_bundle as

  pub fnc marker(): int {
    return 42;
  }

end
)ATTO_INT_BUNDLE"
