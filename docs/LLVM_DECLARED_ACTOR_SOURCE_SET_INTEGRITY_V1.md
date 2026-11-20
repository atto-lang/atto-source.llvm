# LLVM declared-actor source-set integrity v1 (stacked draft)

**Integrity admission only.** It does not parse the cross-file declared actor,
bind its contract, construct sealed state, verify the private external receive
hook, lower critical loans, generate LLVM actor IR or execute guest actors.

## Compiler command

    oreslang-llvmc --verify-actor-source-set-only \
      --expected-source-set-git-sha=e4fd0afea3d21e6fed54f9943abbf0851550e028 \
      /path/to/materialized-counter/actor-source-set.v1

An independently trusted caller supplies the **exact manifest Git blob
SHA-1**. A manifest is not self-authenticating merely because it lists
source hashes. The only successful receipt is:

    ACTOR_SOURCE_SET_INTEGRITY_ONLY units=7 namespace=counter actor=Counter NO_AST_NO_IR_NO_GUEST

## Bounded admission

- Exact v1 schema: one each of package, actor, contract, main, method,
  receive and state source units.
- Manifest <=16 KiB, each unit <=128 KiB, total <=1 MiB.
- Git blob SHA-1 hashes canonical byte sequence blob + space + size + NUL
  + original file bytes, not plain file SHA-1.
- Lowercase 40-character Git hashes, bounded ASCII nominal identifiers,
  canonical relative paths with no dot segments, backslash, drive colons,
  repeated separators or control characters.
- Regular files only, no symlink components, stable descriptor reads and
  UTF-8 checks using the production SourceInput.hpp helper.
- Reject duplicate roles/paths and mismatched file contents before success.
- Verification mode is exclusive with compiler codegen, target, AOT,
  allocator, ownership, RT, isolate and tree-shaking CLI options.

The source-set namespace/actor/contract labels are identity **claims**,
not yet semantic declarations verified by a typed Atto AST. Git SHA-1
object identity is not a cryptographic signature or authorization to
redistribute private source, nor proof of actor-runtime behavior.

## Executable tests and evidence

CTest native_actor_counter_source_set_integrity_only invokes the **actual
oreslang-llvmc binary**. Node materializes the separately pinned seven-unit
shared corpus, but Node does not compile or execute any guest. The compiler
must accept exact source bytes and deny a wrong trusted manifest hash,
modified receive source, source symlink and modified manifest, without IR
or false success output.

The admission header independently compiled in a local GCC C++17 harness
with -Wall -Wextra -Wpedantic -Werror. Its Git blob SHA matched Python
hashlib on three cases. A standalone harness using a minimal read stub
passed positive, wrong trusted hash, modified unit, symlink and missing
unit checks. This is *not* an executed full LLVM compiler/CTest receipt.

## Next compiler parity gates

1. Bind immutable verified file bytes directly to typed frontend parsing
   without TOCTOU rereads or modifying existing single-source behavior.
2. Resolve unique Counter/CounterContract receiver and methods; enforce
   private external receive ingress, one-shot state initialization and
   nonescaping read/write critical loans.
3. Emit compiler-owned actor mailbox DATA, supervisor CONTROL, continuation
   frames, computed replies and generation-fenced STOP cleanup.
4. Execute the exact shared seven Atto units on LLVM O0/O2 with ASan/UBSan
   and Java/GraalVM, proving 1|3|6|10 and two-actor state isolation.

Keep draft. Do not confuse integrity-only gates with actor-parity passes.
Full-tree exact-head executed CI, independent review and any required
private-source carrier authorization remain outstanding.
