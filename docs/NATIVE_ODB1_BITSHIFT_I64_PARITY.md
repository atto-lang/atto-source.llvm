# Native i64 shifts required for portable ODB1 guest byte packing

**Implementation scope:** actual LLVM/AttoLang native source frontend and LLVM IR lowering for signed-i64 `<<`, `>>`, and `>>>`. This is the bitwise primitive used by ODB1 `append_u16` / `append_u32`; it does **not** enable the complete ODB1 module, which also requires independent List<int>, module-linked imports, Result, stdio and guest output parity.

## Java/GraalVM reference

The reference `oreslang-source.java` parser uses `parseShift()` after additive and before comparison. The operator spellings are adjacent-token pairs/triples. Its runtime performs a checked shift-distance conversion: integer values outside **0..63** raise an `IllegalArgumentException`, with Java long semantics for `<<`, arithmetic `>>` and logical `>>>`.

## LLVM implementation

- Preserve individual `<`/`>` tokens in the native lexer. Adjacent tokens are combined **only in expression shift precedence**, not inside generic type parameters; `x > > y` is rejected. `>>>` is recognized before `>>`.
- Introduce three typed AST expression kinds. All scalar-validation paths require **two int/i64 operands** (not bool/string), including pure scalar/constructor/task trees.
- In `LlvmCodegen`, evaluate both operands once, branch on unsigned `distance < 64` (which also rejects all negative signed distances), and invoke `llvm.trap` on invalid counts **before** creating any LLVM shift. Never use masked counts, unguarded shift-by-64 poison, or `nsw` on signed left shifts. `ashr` preserves the sign; `lshr` fills zeros. Left shift intentionally retains Java's 64-bit bit-pattern wrap.
- **Semantic boundary:** Java throws a typed `IllegalArgumentException` for an invalid count. LLVM currently traps, so this is fail-closed on invalid input, **not full exception-class parity**. This patch does not change the native exception ABI.

## Executable regression proof obligations

`tests/native_odb1_bitshift_source_execution.sh` requires an actual built `oreslang-llvmc`, emits real `--target=host` LLVM IR from newly created `.atto` guests, links with Clang and executes:

1. **13 positive guest assertions**: little-endian 65535 byte pack, additive-vs-shift and comparison precedence, signed/unsigned right shift, left wrap, sign extension, 0/63 count endpoints and a dynamic count; exact exit code zero.
2. **Three negative compiler fixtures**: bool value operand, bool distance operand and nonadjacent `> >`; all fail before emitting any LLVM IR.
3. **Four negative runtime fixtures**: counts -1, literal 64, computed 32+32 and INT64_MAX must compile but terminate via the guarded `llvm.trap`, not succeed or time out. The computed case prevents a literal-only check from masquerading as a runtime guard.
4. Inspect the real emitted IR for `shl i64`, `ashr i64`, `lshr i64`, `icmp ult i64` and `llvm.trap`.

The exact-head workflow `.github/workflows/native-odb1-bitshift-guest-parity.yml` builds `oreslang-llvmc` from its own private repository checkout with `BUILD_TESTING=OFF`, then performs these real guest checks. Hosted proof requires a runner and executed steps. A zero-step GitHub Actions failure is runner admission, not compiler failure.

### Independently executed local host oracle

`tests/native_odb1_shift_host_oracle.cpp` expresses the same well-defined signed/unsigned bit operations using explicit uint64 bit patterns. Clang optimized, GCC optimized and Clang ASan/UBSan each executed **19/19** cases locally with no failures, totaling **57 host-only checks**. The file's verified Git blob is `9d1486c2ecd38807717d51bdfd76f370c650931f`.

**These local host checks do not compile AttoLang guest source.** The working container lacks LLVM development headers, so full native compiler build/guest tests require actual hosted CI. No private source has been mirrored across organizations. Do not merge or declare full ODB1, WASM, Cranelift or .NET parity based on the host oracle alone.

## Next steps after the real shift guest passes

Port bounded `List<int>`/owned array APIs, `Result<List<int>,String>`, module imports and `stdio.stdout.write` or an equivalent reviewed guest stdout ABI, and run the exact ODB1 fixture suite with real Java and LLVM compiler artifacts. Only then extend to WASM/Cranelift/.NET under independent source/compiler pins. ODB1 checksum is not authentication, and none of this work enables persistent DB writes or guest ACK.
