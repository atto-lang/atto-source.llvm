# Linux host filesystem broker — security substrate v1

**Status: C11 host-only implementation, not an executable Atto guest, not a multi-tenant sandbox.**

The Linux broker is in native/include/atto/host_fs_broker.h and native/src/host_fs_broker.c. The compiler does not call it. There is no native Atto standard-library binding, signed permission manifest, or TenantVM sandbox here.

- Host policy creation takes a trusted root directory and a trusted policy snapshot (allow-read, allow-write, explicit deny overrides, tenant ID and generation). No grants is deny-all; matching deny always overrides allow. There is one prefix scope per operation in this v1 prototype.
- Resolve paths only relative to the already-open root descriptor with Linux openat2 and RESOLVE_BENEATH, RESOLVE_NO_SYMLINKS, RESOLVE_NO_MAGICLINKS and RESOLVE_NO_XDEV. Unsupported kernels fail closed, with no weakened fallback.
- Read accepts only regular single-link files, with a 1 MiB maximum and no output-data mutation when authorization fails.
- Write stages into a private random temporary file and renames in the pinned parent directory. Symlink and hardlink targets are rejected. Parent directory durability (fsync) is not guaranteed.
- Host I/O requires the *trusted scheduler* to authenticate the actor/tenant identity and generation. Raw integer fields are not cryptographic capabilities and must never be treated as guest-provided authorization.
- The trusted host must independently intersect the signed artifact ceiling, launch request, OS/host ceiling, TenantVM/actor scope and delegation ceiling. No such signed-manifest/CLI verification exists in this C library.

**Adversarial tests:** allowed read/write, explicit denies, scope-prefix confusion, path traversal, symlink escape, hardlink aliases, tenant/generation mismatch, atomic replacement and root-directory rename pinning. The exact uploaded C source/test blobs were compiled and executed locally on Linux using GCC and Clang with -fsanitize=address,undefined. These are host tests, NOT an Atto guest or security certification.

**Limitations:** A hostile native guest with direct syscalls, raw FFI or memory corruption can bypass this C broker. The trusted root must reside in OS directories which adversaries cannot rename or hard-link into; ancestor directory movement and external inode mutation races are not fully proven. Actor/worker-ticket authenticity, manifest integrity, trusted transport, cancellation and revocation, multi-process security, portable Windows/macOS adapters, and async standard-library guest lowering remain unimplemented. Never put this broker's pointers or root file descriptor in guest memory. Do not treat it as host syscall sandboxing.

Run on Linux:

    cc -std=c11 -Wall -Wextra -Wconversion -Werror -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer -I native/include native/src/host_fs_broker.c native/tests/host_fs_broker_test.c -o /tmp/atto-fs-test
    ASAN_OPTIONS=detect_leaks=1 /tmp/atto-fs-test

The source workflow also tests Clang and CMake/CTest, but a source-org Actions run with zero steps is not test execution. Real guest certification requires a compiled .atto fixture, permission manifest validation, denied-before-open evidence, and exact-head verified runner steps.

Related: atto-spec #122, atto-platform-conformance #106, atto-cli #7, hosted std.fs contract atto-spec #117. Do not copy private source to unauthorized CI carriers.
