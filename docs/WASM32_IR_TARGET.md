# Experimental wasm32 LLVM IR target

This is an explicit, pure-core-Wasm compilation target marker, **not** a
complete Wasm object/linker backend. The source compiler still emits LLVM IR,
which can be linked by the separately tested oreslang-wasm/Clang bridge.

    oreslang-llvmc --target=wasm32 tests/fixtures/constant-main.ores > guest.ll
    clang --target=wasm32 -x ir -nostdlib -Wl,--no-entry -Wl,--export=main \
      -Wl,--initial-memory=131072 -Wl,--max-memory=131072 guest.ll -o guest.wasm

Target-marked IR carries the wasm32-unknown-unknown triple and 32-bit data
layout. It does **not** authorize source capability declarations, guest
imports, I/O, actors or async features. Unsupported source fails closed.
Avoid native host ABI/owned-heap features until proven on wasm32.
The default invocation without --target remains unchanged.

Follow-on release gate: build with the source compiler's latest main on a
working private runner, execute the module and compare identical source
fixtures with GraalVM. See oreslang-wasm for provenance and admission.
