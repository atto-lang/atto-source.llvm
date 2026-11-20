# Native HPC/ML proof boundaries (issues #249-#251)

This is a deliberately limited, fail-closed first increment; **no Java/LLVM
distributed ML parity, signed MPA, GPU source execution, or durable sink is
implemented by this change**. Keep these evidence classes separate:

1. `tests/native_hpc_ml_source_evidence.py` compiles an actual `.ores` file
   through `oreslang-llvmc`, links emitted IR through Clang, and executes the
   binary in distinct warmup/measured processes. Results include exact hashes
   of source, compiler, Clang, output executable, host, wall latency and child
   CPU/fault counters. These are **scalar source-bootstrap measurements only**.
   Missing or unsupported source, invalid IR, wrong exit or missing toolchains
   fail closed; there is no C++/Wasm/host-mock substitution.
2. `VolatileFencedSink` is a **non-durable staging guard** for native tests.
   It requires nonempty tenant/resource, rejects obsolete fencing generations,
   conflicts and version exhaustion, and serializes commit attempts. A trusted
   host can initialize it from a verified checkpoint, but it does not persist
   checkpoints, authorize privileged effects, own network ingress, or guarantee
   crash/partition safety. **Never expose it as the durable broker/sink**.
3. `preflight_capability` rejects unsupported versions, missing/foreign
   scopes, expiry, stale policy and stale resource fence. Even valid-looking
   claims yield `SignatureAndRedemptionUnavailable`, not success.
   Cryptographic signature verification and atomic single-use redemption are
   absent. Do not use a boolean signature flag or forgeable host pointers.

## Next acceptance gates

- Select a broker and durable storage contract shared with the Java actor
  backend, with actual native actor-source lowering, versioned envelopes,
  bounded queues, ownership-safe serialization, dedup and outbox recovery.
- Couple per-resource fences/version validation to the **same durable
  transaction** as application effects. Prove partition, restart, replay,
  duplicate, stale-version and split-brain cases; a volatile guard is not a
  substitute.
- Implement maintained-crypto canonical vector verification and a
  non-forgeable executor admission boundary, then atomic redemption across
  Java/native executors. Until that exists, deny all privileged effects.
- Replace the scalar evidence fixture with real supported `.ores` numerical,
  feeder, inference and protected-execution source programs. Compare against
  Java exact-output vectors and publish dedicated/general host provenance.
  Report transport queue/tail latency, GPU/copies, memory, leaks, source actor
  lifecycle and drain/recovery behavior **only once measured end-to-end**.
- Derive SLO thresholds from repeatable dedicated-host baselines, not a CI
  runner. Run sanitizer and mixed-backend fault/recovery matrices before
  marking any of #249, #250 or #251 implemented.

Run a local scalar evidence pass after a normal CMake build:

```sh
python3 tests/native_hpc_ml_source_evidence.py \
  --compiler build/oreslang-llvmc --clang "$(command -v clang)" \
  --fixture tests/fixtures/hpc-ml-scalar-evidence.ores --samples 10 --warmups 2
```
