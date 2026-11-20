# REST demo native admission (independent LLVM backend)

This is a **real native compiler/guest execution** test for the narrow scalar subset, not a working HTTP server. The two source fixtures are byte-for-byte identical to the public REST demo draft PR #78 at `8e9d264db83cdfc1f45df81d907c76ea49fa8684` (positive blob `b1910c892f0ed21011bdcac334e094bd70c3aaf2`; negative blob `d4d9c2fc50fc65c651cca445adb63e668c900f44`). The positive function-derived status scalar must exit 42 after LLVM IR generation, Clang linking and actual process execution at O0 and O2. The negative async HTTP/actor guest must fail compilation without emitting executable IR.

Execute `bash tests/atto_rest_native_admission.sh /absolute/path/to/oreslang-llvmc` with an exact-tree built compiler. The workflow installs LLVM development packages, builds `oreslang-llvmc`, runs both optimized executables, and records negative admission. If private Actions lack minutes, mirror the exact source tree to an authorized private runner with Git blob verification instead; do not claim source-only status as a pass.

Outstanding scope: real HTTP transport, mailbox actors, Spin routing, telemetry, native REST guest, end-to-end AOT, LLVM/Rust five-backend parity, and hard real-time proof.
