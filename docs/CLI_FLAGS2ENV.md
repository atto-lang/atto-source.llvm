# Compiler/runtime CLI flags — flags-2-env v0.3.x

Both Oreslang backends use the canonical upstream project
[flags-2-env](https://github.com/flags-2-env/flags-2-env) and a checked-in
`.cli-flags.toml`. Never copy the unsupported legacy
`ORESoftware/flags-2-env` source.

## Invocation (standalone binaries)

Install the upstream `flags2env` executable first. It must be available on
`PATH`, or set `FLAGS2ENV_BIN` to its absolute executable path.

```sh
# From either repository, run its standalone binary with the canonical parser.
bash scripts/with-flags2env.sh /absolute/path/to/oreslang-binary [oreslang-flags] source.ores
# Audit the schema with upstream validation:
flags2env audit .cli-flags.toml
```

The wrapper invokes `flags2env shell-env --config <repo>/.cli-flags.toml --
<binary> ...`, but **never evaluates** returned shell text. A small
Python 3 boundary parses only shell-quoted assignment values, rejects
unauthorized or repeated environment keys, then invokes `execvpe` with the
original arguments (no shell interpolation). Python 3 must be installed for
this wrapper. Unknown, malformed or oversized export streams, parser errors,
and missing dependencies fail closed. The binary also validates its argv.
The environment is a **configuration source**, not an authorization source.
`[env] load=false` prevents untrusted local `.env` files being loaded.

**Java:** `dev.oreslang.launcher.OresMain` is the combined compiler/run CLI:
`--check` and `--build-analysis` compile/analyze; supplying a source executes
it. The wrapper works for GraalVM Native Image standalone executables. A JVM
invocation with Java VM options (`java -cp`, `-p`, `-m`) must feed
**only Oreslang's guest argv** to `flags2env shell-env` before starting Java,
not the JVM's own arguments. Do not make the Oreslang parser handle JVM flags.
Only non-privileged defaults (execution mode/platform, check and diagnostics)
can be supplied through `ORES_*` env values; scoped permission/capability
grants continue to require OresMain-validated argv.

**LLVM:** `oreslang-llvmc` is currently a compiler that prints LLVM IR.
The checked-in main C++ CLI accepts `ORES_LLVM_TARGET`,
`ORES_LLVM_EXPERIMENTAL_OWNED_INT`, and
`ORES_LLVM_EXPERIMENTAL_RT_SCALAR_LOOP` as defaults which explicit argv may
override. Unsupported `ORES_LLVM_CPU`, invalid booleans, unknown target
triples and duplicate CLI options fail with status 2. There is **no separate
supported native LLVM runtime CLI yet**, so this schema is not evidence of
native runtime command support.

## Before merge

- Run `flags2env audit .cli-flags.toml` with the pinned upstream release and
  run native wrapper tests including malformed flags.
- Run Java Maven JUnit, GraalVM Native Image CLI smoke, LLVM CTest
  `llvm_flags2env_env_argv_contract` and LLVM/Clang guest fixtures.
- Confirm aliases, negations, source positional handling and environment
  precedence are identical across CI and installed binary workflows.
