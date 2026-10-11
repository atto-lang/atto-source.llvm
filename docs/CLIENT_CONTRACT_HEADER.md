# LLVM Oreslang client contract headers

The bounded declaration-only frontend accepts the generated client contract
surface that has the form:

```oreslang
pub define contract ClientContract as
  fnc list_items(String page_size) => String;
end
```

Run `oreslang-llvmc --check-contract path/to/client_contract.ores`.
This checks syntax, name uniqueness, the Java-reference keyword inventory,
and the fixed `String` parameter/return shape. It refuses duplicate method
and parameter names, async syntax, unsupported types, optional positional
arguments, source after `end`, malformed comments and unknown tokens.

This is **not** source-level LLVM executable String lowering. The ordinary
compiler still accepts only its verified native subset and will not silently
discard contracts or produce a fake implementation. The command cannot
authorize publishing an Oreslang SDK or generate TJSV runtime evidence.

Native parity requires String representation and lifetime semantics, actual
RPC client methods, serialization+validation, cancellation and suspension
effects, JNI/FFI boundary tests where applicable, plus cross-backend
positive/negative fixture execution with TJSV current-input admission.


## Experimental verified LLVM contract signatures

`oreslang-llvmc --emit-contract-llvm generated-client-contract.ores` emits
LLVM module declarations with a proposed `%oreslang.string = type { ptr, i64 }`
(pointer, byte length) representation. This path calls LLVM's module verifier
and preserves the exact contract method names and argument arity. The command
never creates executable RPC functions; an external implementation, allocator,
source-enforced ownership, RPC transport, async cancellation, and
GraalVM parity tests remain required. **Do not use LLVM declaration success as
TJSV evidence or admit this target for publishing.**

## Native bounded String codec (experimental ABI)

`NativeString.c` now implements independently executable, bounded UTF-8 owning-string operations: allocate, clone, concatenate, JSON quote, and strict JSON string unquote. The decoder handles escaped control characters and Unicode surrogate pairs, and rejects malformed JSON, invalid UTF-8, aliases to an initialized output, and oversized input. `tests/native_string_test.c` exercises success and negative cases under ASan/UBSan on a private exact-blob CI mirror.

This C runtime primitive is **not** callable from generated Oreslang source yet. The current LLVM contract codegen emits declarations, not RPC method bodies; it does not enforce owning-string moves or suspending network I/O. No Java/GraalVM behavioral parity or native SDK admission follows from the codec test.
