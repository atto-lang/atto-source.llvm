# OresVM native host smoke suite (no LLVM dev libraries)

This project intentionally compiles only the **trusted in-process host** tests,
without configuring the full repository's LLVM-dependent CMake root. It is not
a sandbox, `.ores` compiler conformance run, or cross-backend parity proof.

From a checkout of the exact commit being tested:

```bash
cmake -S tests/oresvm-standalone -B build/oresvm-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build/oresvm-host -j2
ctest --test-dir build/oresvm-host --output-on-failure
```

GCC or Clang sanitizer run:

```bash
cmake -S tests/oresvm-standalone -B build/oresvm-asan \
  -DCMAKE_CXX_COMPILER=clang++ -DORESVM_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build/oresvm-asan -j2
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/oresvm-asan --output-on-failure
```

The suite builds isolate lifecycle/admission, synchronous guest-call
lifetime, aggregate resource limits, **queued shutdown cancellation with
independent other-tenant progress**, and (when `clang` is on PATH and
the host is 64-bit) the test-only hand-written LLVM AOT fixture.

**Not included:** the source-generated `.ores` entrypoint, which requires
the full `oreslang-llvmc` compiler and its LLVM development dependencies.
That fixture is registered in the root CMake project instead. Before
attesting CI, verify that each job really executed steps and that the
checked out Git tree corresponds to the reviewed PR head.

For untrusted tenant code, use a separately validated OS-process
sandbox; this host suite proves no adversarial native-code containment.
