# AttoXP counter: compiler-executed LLVM guest slice

Three `.atto` programs are registered as compiler-backed CTests:
counter main must exit 42, reset must exit 0, and decrement must exit 41.
The LLVM compiler produces IR; Clang links it into a host executable.
These tests are scalar source-to-executable contracts, not host models.

Run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build --target oreslang-llvmc --parallel 2
ctest --test-dir build --output-on-failure -R '^attoxp_counter_guest_'
```

The AttoXP native C reference implements a separate bounded, actual
ABI-linked counter transaction (ADD 40; ADD 2 = 42). This LLVM subset
checks consistent value computation, **not** a linked guest-to-C invocation.
Authenticating native actor identity, FFI calls, UI-thread dispatch,
lifecycle, SDK view mutation and an actual simulator run remain open.

Only executed exact-head compiler/Clang tests qualify as LLVM guest evidence.
A zero-step Actions run is NOT_EXECUTED, even when the source is valid.
