# std/env v1 — LLVM native

This repository independently owns `stdlib/env.atto`. The source candidate deliberately uses native AttoLang declarations and is **not runtime parity evidence**.

## API contract

`get(name): Option<String>`; `has(name): bool`; `get_or(name, fallback): String`; `get_required(name): Result<String,String>`; `get_nonempty(name): Option<String>`; `get_required_nonempty(name): Result<String,String>`; `parse_bool(raw): Result<bool,String>`; `get_bool(name): Result<bool,String>`; `get_bool_or(name, fallback): Result<bool,String>`.

Absent and empty are distinct for `get` and `get_or`. Only explicit nonempty helpers treat an empty value as absent. Boolean grammar is exactly `true`, `false`, `1`, `0`; malformed values are errors even if a fallback exists. Errors must not embed key *values* or secrets. `get_required` makes absence a Result error; denied access is never converted to None or a fallback. No mutation, enumeration, or ambient environment access.

## Backend admission blocker

Native compilation currently rejects unresolved imports; no compiler-visible env bridge is claimed. Native host must receive an explicit ENV key grant, check it before access, and preserve deny-overrides-allow.

A trusted host interface must validate key names, check both coarse isolate ENV capability and scoped grants including explicit denies **before** reading any key, bound memory/UTF-8 inputs, and use deterministic error conversion. Host-only emulation is not .atto execution. AOT/native runs must prove import binding, guest results, denial-before-read, no unauthorized effect or secret exfiltration, and target-specific UTF-8/OS casing behavior.

## Tests

- `pure_bool.atto`: target exit 42 after actual compiler import/link/run; it makes no environment call.
- `denied_fallback.atto`: with an explicit denial of `ATTO_ENV_SECRET_KEY`, the guest must fail before returning any value, including `fallback`.

**Evidence: NOT_EXECUTED.** Fixtures are source-level contract targets until exact-head compiler-backed material CI runs. Never represent these files as implemented host access or full parity.

## Controlled positive guest fixture

`positive_get.atto` requires a deterministic trusted host injection of `ATTO_ENV_EMPTY=""` and `ATTO_ENV_FLAG="true"`, with `ATTO_ENV_ABSENT` explicitly unset. Grant only scoped `ATTO_ENV_*` keys and explicitly deny `ATTO_ENV_SECRET_KEY`. The target is exit code 42, checking present-empty versus missing, nonempty filtering, required-key errors, strict boolean parsing and fallback. Do not use ambient process environment to synthesize a passing result; the compiler must execute the actual .atto guest. **NOT_EXECUTED.**

## New LLVM native host-side prerequisite

- `native/include/atto/host_env_snapshot_v1.h`, `native/src/host_env_snapshot_v1.c`: explicit, per-isolate environment snapshot backed by independently provided allow/deny key rules and a coarse host ENV capability; no ambient `getenv`, `environ`, filesystem or Java access.
- Initialization validates bounded key names (128 ASCII bytes), UTF-8 values (16 KiB), duplicate keys and grants; it copies only admitted values. Guest hot-path `get` and `has` use preallocated storage (no per-call heap allocations). Denied and absent are different outcomes.
- Host `revoke` scrubs retained value copies; `destroy` retires allocations. The trusted embedder **must serialize and quiesce readers before revoke/destroy**. Do not advertise this staging adapter as a real-time-safe concurrent guest capability without actor-generation fencing.
- `native/tests/host_env_snapshot_v1_test.c` and `scripts/test-native-host-env-v1.sh` cover explicit deny-before-read, an invalid denied pointer, snapshot-copy stability, missing versus present-empty, insufficient output buffer, malformed UTF-8, duplicates and retirement.

Run: `sh scripts/test-native-host-env-v1.sh` with a C11 compiler.

**Status: HOST-ONLY source candidate, NOT_EXECUTED on an admitted source-org runner; no `.atto` guest import/linking or LLVM AOT parity proof.**
