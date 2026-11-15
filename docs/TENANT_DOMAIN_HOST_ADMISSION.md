# Native tenant-domain host admission (LLVM backend)

**Status: Trusted-host-only runtime substrate. Not production or native multitenant sandbox.**

## What is implemented

`TenantDomainHost` is a bounded, thread-safe registry with monotonically increasing domain and capability handles. Trusted host code can create/revoke domains, issue symbolic loopback UDP capabilities, submit bounded datagram intents, drain queued intents, and recheck authorization. No OS descriptor is exposed and no actual network syscall happens. Successful submission means **PENDING**, not network delivery. Revocation clears queued intents from the registry; drained copies require an independent trusted broker check.

Local tests exercised cross-tenant denial, quotas, stale capabilities, revocation and 100 concurrent domain admissions. Tests also compiled the C fixture to LLVM IR and linked it to the host implementation, and passed AddressSanitizer/UndefinedBehaviorSanitizer.

## Explicit trust boundaries and limitations

* The authenticated host domain is presently a trusted caller parameter, not a kernel- or hardware-enforced execution identity. Untrusted native machine code could forge it, bypass this library or corrupt host memory. Thus **this is not hostile-native isolation**.
* `revalidate_pending` is diagnostic; it cannot atomically serialize revocation against an external network broker's actual dispatch. Revocation-safe broker authorization and send must be designed and tested together.
* No source-level AttoLang compiler lowering, actor scheduling, heap protection, general Wasm/LLVM security parity, or GraalVM conformance is claimed.
* No actual socket send/recv, native async completion, CPU/memory policing, code hot upgrade, or MPK security proof.
* Default for arbitrary untrusted LLVM code remains process isolation. Runtime and source/compiler work must fail closed where not implemented.

## Verification commands

```sh
clang -std=c11 -Wall -Wextra -Wpedantic -Werror -O1 -g -pthread \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Isrc/main/cpp src/main/cpp/oreslang/runtime/TenantDomainHost.c \
  tests/native_tenant_domain_host_test.c -o /tmp/atto-domain-asan
ASAN_OPTIONS=detect_leaks=1 /tmp/atto-domain-asan
clang -std=c11 -O1 -S -emit-llvm -Isrc/main/cpp \
  tests/native_tenant_domain_host_test.c -o /tmp/atto-domain.ll
clang -std=c11 -O2 -pthread /tmp/atto-domain.ll \
  src/main/cpp/oreslang/runtime/TenantDomainHost.c -o /tmp/atto-domain-llvm
/tmp/atto-domain-llvm
```

**LLVM parity gate remains open:** actual AttoLang source compilation and matching Java/GraalVM execution of byte-identical fixtures, authenticated I/O, constrained guest heaps, and native sandbox enforcement require separate implementation and test evidence.

See the proposed common-source contract in [oreslang-common-source PR #219](https://github.com/ores-truffle-oreslang/oreslang-common-source/pull/219).
