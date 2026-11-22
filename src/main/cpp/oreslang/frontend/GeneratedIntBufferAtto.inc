// Generated from stdlib/alloc/int_buffer.atto; do not edit manually.
// source Git blob SHA-1: 25b3d4bc06a0c859aacadaf0795414027a076bdc
R"ATTO_INT_BUFFER(// Compiler-owned alloc tier source.
// This module requires an allocator provider but no OS, filesystem, network,
// process, actor scheduler, or ambient project dependency.

define module int_buffer as

  pub fnc sum_pair(int left, int right): int {
    let mut Array<int> values = new Array<int>();
    values.add(left);
    values.add(right);
    return values.get(0) + values.get(1);
  }

end
)ATTO_INT_BUFFER"
