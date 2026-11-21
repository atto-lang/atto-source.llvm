# AttoLang LLVM ownership identity inventory — CFG hardening

Status: DRAFT. Production LLVM native CTest and AttoLang guest NOT_EXECUTED.

## Finding and fix
The checker compared BindingId and OriginId across CFG predecessors, but did not validate uniqueness within each predecessor state. The new validate_identity_inventory rejects empty owner names, zero/duplicate BindingIds, and two simultaneously live Value-mode owners of the same positive OriginId. A moved historical source and live destination may retain the SAME allocation origin. Borrowed views may carry the same origin, without independent owner authority. Origin zero remains unknown. The production resolved-place code invokes this check before source authority is used; same_affine_state checks both input states.

## Coverage
The new tests/native_ownership_identity_inventory_30_test.cpp has 30 host C++ assertions against actual production metadata headers: five BindingId cases, five OriginId cases, five CFG join cases, five compiler-resolved place cases, five loan mode cases, five move/alias cases. The native_ownership_join_test.cpp now compares 19 metadata mutations, including two missing ID dimensions. CMake registers native_ownership_identity_inventory_30 and source-owned Actions requires five focused CTests rather than four. Existing 20 scoped compiler tests and 50 source compiler/Clang guest matrix from parent PR remain separately required.

These 30 are compiler-native C++ metadata checks, not 30 additional AttoLang guest programs. Do not claim complete typed MIR, NLL, proven disjoint heaps, actor cancellation, tenant generation fencing, or five-backend parity. No executed production C++/guest evidence until source-org GitHub runner has nonzero steps on exact head/tree with independent review. No private source export.
