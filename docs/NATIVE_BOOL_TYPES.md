# Native source booleans

`bool` is now a distinct checked source type for function/method parameters and
returns, scalar locals, branch locals, and scalar class fields. `true`, `false`,
logical negation, explicit same-type equality guards and `if (flag)` guards lower
to native code. Relative module linking preserves boolean type metadata.

```ores
pub define class Flag as
  let bool enabled = true;
  pub read(): bool { return self.enabled; }
  pub set(mut self)(bool value): void { self.enabled = value; return; }
end
```

The source type checker runs after name, method and ownership validation and
before LLVM emission. It rejects integer truthiness, bool/int arithmetic,
argument/return/initializer mismatches, incompatible scalar loans, bool values in
Array<int> or actor integer mail, and bool specializations of the currently
int-only generic slice. A boolean cannot be smuggled into an integer through a
class field, copied loan, nested call, guarded return or module boundary.

Native storage/ABI slots remain i64 with boolean values encoded as 0 or 1;
logical negation uses XOR with 1. This is not a C `_Bool` ABI. Trusted foreign
callers must obey the source signature and canonical boolean encoding. Existing
source-public functions are not an untrusted FFI boundary. No bool/int conversion
or type-based overload selection is introduced. Borrow exclusivity and moved
place checking remain in the existing ownership checker.

Class declaration initializers retain the existing literal/pure constructor
restrictions; native explicit constructor parameters are still int-only. Objects
remain stack-owned scalar layouts. There is no new allocation, reference counting,
tracing GC, interpreter or JIT. General String/Array object surfaces, closures,
Future/await, HTTP capability ABI and actual Spin/OTel lowering remain unfinished.

`native_bool_types.py` executes admitted programs natively, repeats them under
ASan/UBSan, and can execute the same sources with `--java-command`. Negative
controls assert the intended type/loan diagnostic and no partial IR. The native
HTTP helper additionally reads a private bool field and uses its negation in a
guard, exercising this feature through real actor-owned HTTP requests.

Local validation: 423/423 CTests passed, including Release and ASan/UBSan bool
and actor HTTP cases. The 36-case bool suite includes twelve identical-source
Java/native executions.
