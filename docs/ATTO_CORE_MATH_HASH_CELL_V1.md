# LLVM-local core math/hash/cell v1

The LLVM backend owns independent `.atto` sources in `stdlib/`.
`atto.core/{math,hash,cell}` and `ores.core/{math,hash,cell}`
resolve to the same baked source and guest unit. The Rust generator pins
all three source Git blob SHAs and exact embedded C++ literal bytes.

- Math is signed integer min/max/signum/closed range; `Result` clamp and
  float intrinsics are deferred until type and AOT admission.
- Hash is seed=17, polynomial byte fold modulo 1,000,000,007
  without native modulo, overflow or wrapping. Invalid input = -1.
  This is **not** cryptographic and doesn't implement full Hash trait.
- Cell is borrow state *policy*, NOT compiler-enforced `RefCell<T>`.
  No runtime storage, mutable aliasing, atomicity or capacity authority.

The C++ test checks registry/source identity, not guest execution.
`tests/core-math-hash-cell-v1/guest.atto` is a proposed AOT canary.
Do not claim guest compile/execution or cross-backend parity until the
current-head LLVM compiler builds and executes that .atto fixture.
Related LLVM Math.abs/Math.neg PRs #547/#548 are independent.
