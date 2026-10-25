# Native scalar instance methods

The compiler now lowers synchronous instance methods on bounded scalar classes.
`pub define class` exports a class for qualified use through a selected module;
private classes remain usable within their own source unit. Class fields retain
normal visibility, final-slot and ownership checks.

```ores
pub define class Counter as
  let int value = 40;
  pub read(): int { return self.value; }
  pub add(mut self)(int amount): void {
    self.value = self.value + amount;
    return;
  }
end
pub fnc main(): int {
  let mut Counter counter = new Counter();
  counter.add(2);
  return counter.read();
}
```

Calls bind by method name and explicit argument count, then acquire a hidden
receiver loan. Default receivers are read-only; `mut self` requires an exclusive
borrow. Generated method functions use internal LLVM linkage and retain class
identity through module qualification. Only a method's exact declaring class
receives lexical private-field/method privilege. Forwarding its receiver to an
ordinary function does not forward private access. Private methods are checked
before lowering; const/final fields and moved or conflicting owners remain
restricted. `self` is compiler-bound, not an ordinary user-created capability.

Same-file and imported public classes, method overloads, scalar constructors,
private helper calls and shared/exclusive reborrowing execute as native code.
A public constructor requires a public class; imported private constructors do
not become public through a module alias. Implicit default constructors retain
the existing zero-argument scalar behavior. Fields must precede an explicit
constructor, but methods may follow it. An ordinary class method named `send`
binds as an instance call; actor-reference sends retain their existing mailbox
admission/order checks.

This is not general object/async lowering. Classes remain stack-owned with up
to 16 scalar fields; returning objects, nested object fields, inheritance,
virtual/static/generic methods, escaping bound methods, String/Array receiver
storage, actor placement and suspension remain unimplemented. Explicit read
receiver syntax, field/method name collisions and effect-qualified methods are
rejected until their typed rules are lowered. Mutable receivers are borrowed
before arguments; two-phase receiver reservation is not claimed, so a nested
argument that borrows that same mutable receiver fails closed.

The native HTTP status helper now instantiates a private `StatusPolicy` class
and calls a method that reads its private constant field. LLVM compiles the
module and method bodies into the artifact; no method/source interpretation or
JVM occurs while serving. The trusted C++ HTTP adapter is still responsible for
framing/routing/formatting. Full Spin/HTTP/OTel compilation remains incomplete.

`native_source_class_methods_release` and its ASan/UBSan counterpart cover 33
cases, including nominal mismatch, private fields/methods/constructors, receiver
loans and actor-send disambiguation. Eleven identical positive programs also run
in Java. Three qualified imported-construction cases are native-only evidence:
the current Java runtime rejects `new Lib.Class()` despite parsing/typechecking
that form. This is recorded rather than presented as cross-backend parity.
