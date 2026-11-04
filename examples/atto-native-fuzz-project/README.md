# Native LLVM fuzz adapter (AttoLang M0)

This project is a runnable input to the **merged Rust** `atto test --fuzz`
runner. Build the byte-level native target from the top of the LLVM repo
using the real LLVM compiler (no private-source mirror):

```sh
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target oreslang-llvmc oreslang-no-gc-ir-audit-build --parallel 2
mkdir -p examples/atto-native-fuzz-project/build
bash scripts/build-native-atto-fuzz-target.sh \
  tests/fixtures/atto-fuzz-byte.atto \
  examples/atto-native-fuzz-project/build/fuzz-byte
atto test --root examples/atto-native-fuzz-project --fuzz --seed 1729 --cases 1000
```

This expects a separately installed `atto`/`atto-test` from the Rust
test CLI. The `.atto` guest defines `fuzz_byte(int octet): int`; the
native C adapter owns bounded stdin reads and independently checks
the guest result equals `octet + 1`. LLVM has not thereby implemented
general-purpose AttoLang stdin I/O.

To prove the failure oracle, build the intentional mutant to the same
output and rerun the CLI. It must reproducibly fail, minimize the input
and save a byte reproducer under the project-local failure directory.

```sh
bash scripts/build-native-atto-fuzz-target.sh \
  tests/fixtures/atto-fuzz-byte-mutant.atto \
  examples/atto-native-fuzz-project/build/fuzz-byte
atto test --root examples/atto-native-fuzz-project --fuzz --seed 1729 --cases 8
```

Optional native ASan+UBSan (separately verify sanitizer positive controls):

```sh
ATTO_FUZZ_SANITIZERS=address,undefined \
  bash scripts/build-native-atto-fuzz-target.sh \
  tests/fixtures/atto-fuzz-byte.atto \
  examples/atto-native-fuzz-project/build/fuzz-byte
```

Build and run evidence is **pending** while GitHub Actions fails before
job steps. This is not live actor race detection, coverage-guided fuzzing,
or full input API parity.
