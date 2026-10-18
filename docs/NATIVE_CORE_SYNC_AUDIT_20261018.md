# Native synchronization audit — 2026-10-18

This branch implements allocation-free, nonparking C11 atomic mutex and RW admission gates in `src/main/cpp/oreslang/runtime/OresCoreSync.h`. It is a compiler/runtime core substrate, not yet the guest-source `Mutex<T>` or `RwLock<T>` standard library.

Tests in `tests/native_core_sync_test.c` cover concurrent access, invalid and stale releases, generation exhaustion and writer cancellation competing with acquisition. The core implementation does not depend on Java, pthread mutexes, an allocator or an OS wait. The test harness uses pthreads to exercise contention.

Validation: [private native exact-blob CI run #39147733116](https://github.com/sagitta-stack/sagitta-docs/actions/runs/39147965114), passed both GCC and Clang paths: Git blob verification, freestanding object compilation, Release/NDEBUG threaded stress, ASan/UBSan threaded stress and GCC ThreadSanitizer stress.

Exact verified source blobs:
- `OresCoreSync.h`: `b0f355d4c1364962cef2550ef2d17237ddaf63d8`
- `native_core_sync_test.c`: `20953f29edb24e6ecbd87bb19b20d2380035ccf1`

This is focused exact-blob verification, not full-tree LLVM CI. Remaining blockers: native guest-source ABI/lowering; linear guards and borrow checking; stdlib scheduler FIFO waiting and cancellation; actor `state` RW integration through sealed `handle_state`; multicore supervisor/actor race testing; TSan and target-specific freestanding executable proofs. Do not enable actor max>1 solely from these core tests.
