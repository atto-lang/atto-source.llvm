# LLVM-owned typed AOT macros in .atto (imported M1)

**State:** source and AOT tests authored, not executed. No real compiler, CTest,
Clang guest, sanitizer or production-hardening pass is claimed yet.

The three independent compiler-local `.atto` producer files are
`stdlib/core/macros.atto` (`const_int`, `splice_expr`),
`stdlib/std/macros.atto` (`const_retry_count`, `const_timeout_ns`) and
`stdlib/alloc/macros.atto` (`const_capacity`, `const_alignment`).

All macros are compile-time only. `Const<int>` must be closed, checked signed
64-bit arithmetic; `Syntax<Expr>` preserves the original expression AST. No
runtime macro engine, source eval, AST-to-text expansion or dynamic compiler
is emitted into the native guest. Scalar profile: eight macro declarations,
128 expansions, 512 constant AST visits, inclusive depth 64. Neither
`const_capacity` nor `const_alignment` validates runtime range, alignment
power-of-two, quota or allocator authority; consumers must check those.

Native M1 currently permits only same-directory explicit public macro producers:
`import * as m from "./macros.atto";`. No automatic module import,
`+/std` resolution, mixed executable declarations in producer or general
package linking is asserted. Each positive sample lives adjacent to its
producer to exercise the actual restricted linker without broadening it.

Six CTest targets compile each library consumer to LLVM IR, link it using
Clang, execute a guest returning 42, require silent stdout/stderr and hash
both physical source files, compiler, IR and binary. Three negative tests
require a macro-specific compiler denial with **zero** emitted LLVM IR:
wrong argument kind, dynamic runtime input, signed-64 overflow. All must run
on exact-current-head funded source-owned runners; zero-step failures are
NOT_EXECUTED. Preserve independent authorized source-export governance.

This feature branch stacks on the imported native macro M1 PR #619 and its
ancestors; keep it unmerged until exact-head AOT and security evidence exists.
