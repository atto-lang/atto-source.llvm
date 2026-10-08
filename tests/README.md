# Source-to-LLVM-IR compatibility fixtures

All positive fixtures are a deliberately tiny parseable subset of the Java
Oreslang grammar. Negative files must fail with nonzero exit.
CTest checks both behavior and LLVM-IR fragments. Expand this to Java parity
before admitting general expressions or actor code.
