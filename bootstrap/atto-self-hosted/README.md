# Atto-written compiler: bootstrap seed (NOT self-hosted)

This directory stages the future independent `atto-lang/atto-source.atto`
compiler until that dedicated repository can be created. **It is not an
LLVM implementation or a Forge implementation.** The intent is to preserve
a history-carrying Atto-authored seed and move it to its own repository.

**State: Stage-0 source seed only. No executed Atto guest proof is claimed.**

Tracking spec: https://github.com/atto-lang/atto-spec/issues/70

## First Atto-authored compilation kernel

`src/scalar_codegen.atto` is an intentionally tiny source-to-code
compiler for the unambiguous three-ASCII-byte expression grammar:

```ebnf
expr  = digit, ("+" | "-" | "*"), digit ;
digit = "0" ... "9" ;
```

Its `compile_ascii_triplet` function accepts the three source bytes as
signed integers, validates the digits and operator, performs checked
constant-expression evaluation, then returns a packed three-byte **Wasm
function instruction fragment**:

```text
0x42 <signed-LEB128-i64-constant> 0x0b
     i64.const immediate         end
```

It supports only results in [-64, 63], which have a one-byte signed-LEB128
encoding. Each successful return is a positive packed i64. Negative
returns are explicit errors, not generated code. **This is not a complete
Wasm module**, not a general AttoLang frontend, and not self-hosting.
A future module writer must emit the magic/version, type/function/export
and code sections with resource and ABI checks.

This seed deliberately avoids strings, arrays, files, heap allocation
and unsupported managed APIs until Stage 0 has executable evidence.
Canonical AttoLang syntax uses `define pub fnc`, snake_case functions,
`eq` for value equality, and explicit returns.

## Two independent gates

1. Host reference tests: `rustc --edition 2021 --test
   bootstrap/atto-self-hosted/tests/scalar_seed_model.rs -o /tmp/atto-seed-model
   && /tmp/atto-seed-model`.
   These check expected bytes, signed LEB128, rejection and distinct
   inputs. **They do not execute the Atto source.**
2. Guest admission: build `src/scalar_codegen.atto` using a pinned real
   AttoLang compiler and execute the produced artifact. The `main`
   function has expected native return **42** (the encoded result is
   731714 and the observed entry result subtracts 731672). Record exact
   checked-out source commit/tree, compiler binary SHA, toolchain,
   test output and executed CI steps. A compile-only test is not enough;
   this exact guest test is **NOT YET VERIFIED**.

Sample target-side commands, **not claimed executed**:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/oreslang-llvmc bootstrap/atto-self-hosted/src/scalar_codegen.atto > /tmp/atto-seed.ll
clang /tmp/atto-seed.ll -o /tmp/atto-seed
/tmp/atto-seed
code=$?
test "$code" -eq 42
```

If the current LLVM frontend rejects the seed, do not silently fall
back to an interpreter or stamp the gate passed. Fix the compiler gap
with separate positive and negative native tests.

## Roadmap

See `docs/BOOTSTRAP_GATES.md`. The next required functionality is
source input as a byte view, repeatable tokenization, AST allocation and
typed diagnostics, followed by code emission of complete Wasm/native
artifacts, then the actual stage-1/2/3 bootstrap.

The seed should be migrated to `atto-lang/atto-source.atto` with
tracked commit provenance when the GitHub repository is created.
