# LLVM OTP module import gate (pending typed linking)

Native LLVM currently has no typed source linker for the Oreslang OTP
distribution and **must not claim OTP parity** because it parses an
`import` token. The acceptance gate contains:

- one known-working native scalar source-to-LLVM control fixture;
- 6 *canonical* `otp/*` modules and 6 legacy Java `std/otp/*` spellings,
  each rejected with no emitted LLVM IR and the exact diagnostic;
- four unknown/traversal-shaped imports, likewise rejected.

The native compiler must reject all sixteen paths until it can resolve
compiler-owned resource identities, link typed Oreslang modules safely, and
emit verifiable LLVM IR. Silently accepting imports into empty modules or
treating imported declarations as handwritten C stubs is prohibited.

After module linking exists, replace this gate with positive equivalent
Java/LLVM source fixtures and negative tests for unknown path, aliases,
shadowed filesystem packages, type/effect mismatch and transitive import
closure. Do not remove fail-closed behavior early. The native actor
scheduler, CONTROL, reply futures, cancellation, and supervision lifecycle
are **separate** parity gates.

The test executes a real `oreslang-llvmc` only when the native binary is
actually built. No GitHub Actions green result is asserted here.
