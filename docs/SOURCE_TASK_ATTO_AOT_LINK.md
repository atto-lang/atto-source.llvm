# Native .atto Task<int> AOT linkage

Normal `scripts/compile-aot.sh` now links `SourceTaskRuntime.c` and `NativeActorScheduler.c` when the real emitted LLVM IR declares `oreslang_source_task_*`. Previously the dedicated source Task test linked them explicitly, leaving the standard AOT compiler path without the Task symbols.

Run `EXPECTED_HEAD="$(git rev-parse HEAD)" bash scripts/test-source-task-atto-aot.sh` after building the real `oreslang-llvmc` and no-GC IR auditor. The script checks the unmodified `.atto` copy of the existing native Task fixture, generated IR symbol references, native O0/O2 Clang links, exit 42, and negative no-artifact admission. This remains **NOT EXECUTED** until a source-local runner actually runs it. The known native Task slice is restricted to one capture-free `Task<int>` in synchronous main; generic futures, cancellation, and Java parity remain separate work.
