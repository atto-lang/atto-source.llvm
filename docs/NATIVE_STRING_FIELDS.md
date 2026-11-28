# Native routing records and String fields

LLVM supports declaration-only immutable scalar fields with an implicit,
positional constructor. This admits the actual `http-routing` PathSegment shape:

```ores
define class PathSegment as
    pub val int kind;
    pub val String value;
end
```

`new PathSegment(1, "name")` initializes every declaration-only field in field
order. Constructor types are retained independently of the physical i64 slots:
String and bool arguments cannot be substituted for int, or vice versa.
Initializer expressions use private parameter slots before the class owner is
published. Missing arguments reject before IR. Direct field reads may initialize
another record only after the ownership pass proves the source place is live and
accessible. A partially or wholly moved source is still rejected.

String fields can also have literal declaration-site initializers. Their reads,
borrowed receiver methods and assignments retain the String type. Existing final
field, privacy and class ownership checks apply; String handles cannot escape as
integers. General explicit constructor assignment bodies remain unsupported.

Records use bounded native stack layouts. Immutable String values remain in the
bounded synchronous invocation region, which closes deterministically without
tracing GC. Creating a second record from a String field safely retains the value
until that region closes; no class field owns a raw pointer or frees shared bytes.
This is synchronous region lifetime support, not actor/continuation lifetime
lowering. String fields still reject on unsupported targets and actor/await paths.

The differential fixture constructs and copies the PathSegment declaration and
reads its String through a borrowed getter, then executes the same program on
Java and native LLVM. This is a real routing representation prerequisite, not a
claim that the complete router or Spin HTTP server compiles yet.
