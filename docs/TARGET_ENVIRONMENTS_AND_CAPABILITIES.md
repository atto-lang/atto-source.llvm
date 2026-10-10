# Oreslang target environments, backends and capabilities

Draft contract, October 10, 2026. This document describes both the implemented
Java opt-in admission API and future cross-platform work. The LLVM compiler
does not yet support cross-file imports, environment admission, or the proposed
output backends. Unsupported features must fail closed, not pretend to work.

## Independent dimensions

Environment: server, desktop, browser, ios, android, embedded, edge-js, edge-wasi.

Backend/output: Graal/Truffle, JVM classfiles, Android DEX, LLVM native, core
WebAssembly, WASI components, JS/TS, C, custom VM bytecode.

Capabilities/host effects: clock, random, net.client, net.server, fs.read,
fs.write, threads, interop.java, interop.native, device.camera, device.gpio,
dynamic.code, heap, gc.

A browser-safe declaration is not a proof of trust. The runtime host must still
enforce all declared permissions, quotas, and interop policies.

## Current Java opt-in header contract

A participating .ores file MUST begin, on its first physical line, with:

    // ores:targets=browser,edge-js,android,ios,server
    // ores:requires=net.client,clock
    import fnc fetch_data from "./portable/http.ores";

The second line is optional; omission means no requirements *declared*, not
that effects have been inferred or verified. The directives are currently lexer
comments for grammar compatibility; normal runs ignore them.

The Java API is dev.oreslang.compiler.TargetAdmission.validate(entryUnit,
sources, target, grantedCapabilities), or its overload with pinned import
resolutions. The Java launcher now has an opt-in validation-only mode:

    oreslang --check --target-env=browser --target-capabilities=net.client,clock src/main.ores

This mode uses the actual LinkedProgramRunner import closure and its pinned
resolutions. It does not execute the program or produce a browser artifact. It checks the actual parsed Ast.ImportDecl objects and uses
ImportGraph.resolveImportUnitId. For each unconditional import A -> B:

1. targets(A) must be a subset of targets(B).
2. requires(A) must be a superset of requires(B).
3. Every reachable unit must allow the selected build environment, and its
   requirements must be included in the caller-granted capability budget.

Import cycles are allowed and individually checked. Missing/misplaced headers,
unknown values, duplicates, unresolved imports, unsupported .java islands and
unrecognized targets fail closed. Java host imports require a server/desktop
target and an explicit interop.java declaration. Execution still additionally
needs IsolatePolicy authorization and a class allowlist.

**LIMITATIONS:** This checker remains opt-in via --check and is not enabled by
default for execution or other build modes. It is not integrated with code
artifact emission, stdlib effect inference, dynamic imports or compiled LLVM. Declared effects can lie until compiler semantic
effect inference is introduced. Do not advertise standard builds as safe solely
because this API exists.

## Example rejection

    // ores:targets=browser,server
    import fnc read_secret from "./server/fs.ores";

If server/fs.ores declares targets=server only, the importer has made a false
promise and compilation under target admission fails, including during a
server-targeted build. Per-target conditional imports will need their own
validated, non-escalating semantics.

## Future package policy proposal, not accepted by current manifest parser

    [target]
    environment = "browser"
    backend = "wasm"
    abi = "web"
    features = ["simd"]
    max_heap_kib = 8192

    [capabilities]
    allow = ["net.client", "clock"]
    deny = ["fs.write", "interop.native", "dynamic.code"]

    [package]
    default_targets = ["browser", "server", "edge-js"]
    require_file_header = true

Current .oreslangc.cfg.toml schema_version=1 must NOT silently accept the
above unsupported fields. Introduce a schema change when implementation lands.
File declarations should only narrow package target sets; capability budgets
are upper bounds granted by the build/host, and files must never expand them.

## Required future compiler semantics

- Infer effects from calls, native/host imports, stdlib signatures, indirect
  function pointers and actor handlers. Verify the transitive dependency graph
  including reexports, generated code, package manifests and all import edges.
- Reject dynamic imports that cannot be proved safe; compile-time conditional
  imports must be specialized *before* checking the active import graph.
- Keep runtime enforcement: Wasm imports, native syscalls, Java interop, and
  JS host adaptors must not gain power merely because a file says it is safe.
- Validate CPU architecture, ABI, WASI version, wasm feature flags, memory
  upper bounds, stack requirements, no-GC ownership proof, and available
  threading separately from the declared environment.
- Produce reproducible artifact and dependency capability manifests with import
  traces and effective capability budgets, then verify them at loading.

## Suggested initial outputs by environment

| Host | Output to prioritize | Constraint |
| --- | --- | --- |
| Browser | JS/TS or core Wasm plus JS adapter | No general host filesystem/WASI |
| Edge JS isolate | JS/TS, optionally Wasm | Provider API and resource limits vary |
| Edge WASI | WASI component model | Pin ABI, WIT and WASI host version |
| Android | Native AOT and/or JVM-to-DEX | Android ART uses DEX |
| iOS | LLVM AOT with C/Swift/Obj-C interop | Code signing and store requirements |
| Tiny embedded | Native AOT, C or tiny interpreter | No OS/heap/threads on some devices |
| Larger embedded | Native or small Wasm runtime | Wasm runtime memory may be too large |
| Server/desktop | Graal, LLVM, JVM | Select according to performance profile |

LLVM already has a WebAssembly target; no independent wasm source-language
compiler is necessary as the first step. WASI 0.2 is the interoperability
baseline for broad component support; WASI 0.3 adds async functions, futures
and streams. Neither guarantees a portable actor scheduler, cancellation,
shared memory, GC-free allocation, or network support without host integration.

## Required merge gates

1. Run Java TargetAdmissionTest on the exact PR head.
2. Run Java TargetAdmissionLoaderTest and CLI smoke tests, confirming that
   --check --target-env uses exact import IDs and ordinary server validation
   still works without headers. Target-check mode is currently validation-only.
3. Add package policy with strict versioning and compiler-inferred effects.
4. Add native LLVM parity or explicit negative diagnostics, not permissive
   fallback.
5. Add end-to-end browser, edge WASI, iOS, Android and embedded artifacts
   with runtime-permission and cross-target CI smoke tests.

No implementation or test result should be implied beyond the actual verified
state of the relevant repository.
