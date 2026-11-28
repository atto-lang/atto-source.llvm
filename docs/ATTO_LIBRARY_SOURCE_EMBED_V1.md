# Canonical AttoLang Core/Alloc source embedding — Rust gate

Three portable AttoLang modules are checked in as the canonical **guest source**:

| Module | Canonical file | Git blob SHA-1 |
|---|---|---|
| `atto.core/numeric` | `stdlib/numeric.atto` | `5ced991f1d49b3c2a60fdd72b9003c0a4d18cd72` |
| `atto.alloc/int_buffer` | `stdlib/alloc/int_buffer.atto` | `25b3d4bc06a0c859aacadaf0795414027a076bdc` |
| `atto.alloc/int_buffer_bundle` | `stdlib/alloc/int_buffer_bundle.atto` | `07c7083e099a18f1db7097f9639efc677070eb95` |

The Rust generator embeds **exact bytes**, not a semantically rewritten
host-language implementation. C++ includes generated expression-only raw
literals; the checked-in includes cannot drift from source without failing
`cargo run --locked --manifest-path tools/atto-library-embed/Cargo.toml -- --check`.

Use `--write` only to intentionally regenerate derived files. Changes to
canonical source require updating the pinned blob and reviewed conformance
contract first. Canonical `atto.*` aliases and historical `ores.core/*`,
`alloc/*` resolve to the exact same compiler unit source and identity.

**Coordination:** LLVM #1428 previously staged a Python generator for
`stdlib/numeric.atto`. This PR preserves its exact Git blob and C++ literal
format but uses Rust, extends generation to Alloc, and stacks on LLVM #1454
(current main integration). Do not merge both generator implementations
unmodified.

**Evidence:** generator tests, Git blob read-back, and C++ compilation are
host/source evidence; they do not prove native guest execution. The separate
LLVM Cortex-M gate must be run on exact head for that proof.
