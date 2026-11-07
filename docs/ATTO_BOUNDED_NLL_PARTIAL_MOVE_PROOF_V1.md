# AttoLang stage-zero NLL and partial-move CFG contract

DRAFT; source-authored; production LLVM CTest and AttoLang guest NOT_EXECUTED.

OwnershipCfg.hpp adds an explicit Reinitialize for definitely moved, unborrowed places. MaybeMoved, still-owned and borrowed places fail closed. Source lowering must still emit a real field write; this static transition does not synthesize code. The explicit UseSharedLoan/UseExclusiveLoan operations verify exact live lexical tokens and correct loan mode. Suspend forbids live loans until the compiler has proved how they survive suspension and cancellation.

OwnershipLastUse.hpp contains a bounded, NOT GUEST-INTEGRATED adapter. It accepts only one straight-line CFG block, no loop or branch, explicit nonzero loan identities, complete borrow-site and final-use-site records, no mixed explicit release and no later use. It emits one release after a claimed final use then reruns the existing powerset verifier. Completeness CANNOT be authenticated from a caller-supplied proof; typed MIR/SSA use-def analysis must mint it before any backend uses this function for source admission.

30 new host-native C++ cases cover 10 move/reinit tests, 8 suspension/loan tests and 12 explicit last-use tests. CMake and exact-head LLVM Actions require six native ownership CTests, alongside the parent's compiler/Clang guest matrix. These 30 are native CFG tests, not 30 additional .atto programs.

Outstanding: actual source-owned runner admission, immutable tree identity, production C++ build, compiler-backed .atto execution, real typed-MIR use-site completeness, exceptional edges, actor cancellation/stack/heap and five-backend parity. Keep DRAFT until actual test and independent review. No private source copied to other organizations.
