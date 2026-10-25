# Merged GPU admission: exact-source validation gate

PR #761 was merged to main at 29ba4f9476201ddaa9c4e07046a37106f5b0e0c9. Prior PR-head Actions runs reported failure, not passing compiler evidence.

## Required tests on exact merged Git tree

```sh
cmake -S . -B build-gpu-gcc -DCMAKE_BUILD_TYPE=Debug
cmake --build build-gpu-gcc --parallel 4 --target oreslang-dop-gpu-admission-test
ctest --test-dir build-gpu-gcc --output-on-failure --no-tests=error -R '^dop_gpu_exact_layout_fence_admission$'
cmake -S . -B build-gpu-clang-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_CXX_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer' '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
cmake --build build-gpu-clang-asan --parallel 4 --target oreslang-dop-gpu-admission-test
ctest --test-dir build-gpu-clang-asan --output-on-failure --no-tests=error -R '^dop_gpu_exact_layout_fence_admission$'
```

Record the exact tree, compiler identities, logs, and nonempty executed job steps. An authorized private carrier must have protected source/carrier main and separately reviewed destination/workflow/test-plan pins. Do not transfer private source before approval.

The queue receipt is factory-only and seals queue identity and fence tickets. Metadata preflight is not provider attestation: provider-side generation, tenant, device, quota, cancellation and trusted completion checks remain mandatory. A failed native test requires a corrective PR, not a synthetic substitute.
