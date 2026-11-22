# AttoXP LLVM scalar guest candidates

These independently authored AttoLang `.atto` fixtures use currently admitted
`name: int` parameter syntax in the LLVM branch. Canonical type-first
`int name` parsing has separate unmerged work (LLVM PR #1348), and is NOT
silently considered supported here.

CMake `oreslang_contract_test(... --run 42 --clang ...)` must compile each
positive fixture using the real `oreslang-llvmc` frontend, emit LLVM IR,
link with Clang and execute the native guest returning 42.

The three negative fixture commands must return a nonzero exit and empty
compiler IR stdout. They cover removed modulo, unknown function and bad
arity; generic parsing failure is insufficient to prove a specific typed
diagnostic until separately tested.

No private source tree was copied from AttoXP, and tests have NOT EXECUTED
under the source organization's current hosted CI admission failures.
These are scalar-call tests, not UI ABI/native actor integration.
