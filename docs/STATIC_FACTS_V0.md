# Common advisory static facts v0 — LLVM native bootstrap

`python3 tests/static_facts_v0.py --compiler /absolute/path/to/oreslang-llvmc
--source /absolute/path/to/file.ores` admits **only** one literal-returning
`main():int` in a closed ASCII, comment-free source grammar. It stages
the exact bytes in a private directory and runs the real LLVM compiler.
A success exit and bounded, recognizable scalar LLVM IR are mandatory before
the JSON is emitted.

Unlike the rich LLVM compiler, this experimental v0 bridge cannot yet export
real callsite multiplicity, interprocedural actor spawning, loop lowering,
callbacks or ownership proofs. It rejects all such source, rather than
misreporting a zero-effect program. Exact SHA-256 is source identity ONLY,
not attested producer/compiler provenance. The one `__root__.main/0`
procedure has no loops/edges under this narrow admitted grammar.

CTest includes self-test negatives and a real compiler test:
`ctest --test-dir build -R llvm_static_facts_v0 --output-on-failure`.
Do not claim static WCET, actor heap isolation, termination, safety or
full AOT profile certification from this advisory JSON.
