# Native owned-i64 lifetime-token security boundary

The experimental `--experimental-owned-int` backend uses a host-owned
`OresOwnedI64Region*` and globally monotonic, never-recycled `uint64_t`
`OresOwnedI64Handle` tokens. These close the stale-address ABA hole caused
by allocating a new value in a freed slot: old tokens do not match the new
allocation, even if native slot addresses repeat or another region is used.

## Scope and trust

- **Not a bearer security capability.** Tokens are predictable integers, not
  random secrets, MACs or proof of authorization. The compiler must prevent
  source code from forging token values or accessing another owner's region.
- **Not a sandbox or IPC ABI.** Never send the C region pointer, token, or
  unchecked native operations to untrusted guest code, a separate tenant,
  or a remote machine. Token uniqueness is only per process lifetime.
- **Serialized owner only.** Region allocation/read/drop/close mutate memory
  without an internal lock; the compiler/runtime must enforce one logical
  owning actor turn and no concurrent FFI access. A handle is invalid after
  drop or physical region retirement.
- **Process-fatal misuse.** Invalid/stale tokens, double drops, full regions,
  nonempty close and token exhaustion call `abort()`. This is fail-closed for
  trusted compiler-generated code, **not** a recoverable error surface for
  hostile network/API inputs. Validate external data before entering native
  ownership code; use an OS isolation boundary for untrusted actors.
- **No general class ownership yet.** Only the bounded experimental scalar
  `owned int` lowering is admitted; nontrivial class fields, async mutable
  loans, source `await`, general FFI ownership and Wasm native heap regions
  still fail closed.

## Verification

`tests/native_owned_region_security_test.c` uses
`require_or_abort()` instead of C `assert()`, so security checks execute
in `-O2 -DNDEBUG` builds. Positive cases demonstrate allocation, explicit
move/drop, distinct sequential handles and slot reuse. Fork-isolated negative
cases exercise stale read/drop, wrong-region read/drop, double drop, zero and
sentinel tokens, region capacity, nonempty close, and the test-only exhaustion
hook. `ORESLANG_NATIVE_TEST_TOKEN_EXHAUSTION` is only defined in the isolated
test executable; the production object must not export the setter.

The exact compiler tree still requires complete CMake/CTest, native generated
code under Clang ASan/UBSan, and strict Java/LLVM conformance before this
branch is merged. Passing the focused C security fixture does not prove
source-language or cross-backend memory safety.
