# Synchronous routing index loops and structured returns

Host LLVM source supports `for (let int index = initial; condition; index = next)`
with a scoped integer counter. Initial runs once; condition runs before every
iteration; next runs after each falling-through body. Empty ranges execute no
body or update. Nested scopes use entry-allocated slots, so iteration does not
allocate an unbounded native stack. The named update target must be the loop's
own counter. Arbitrary scalar assignments, break/continue and async suspension
remain unsupported in this synchronous lowering.

Nested conditions accept both brace syntax and the routing library's
`if condition; then ... fi`, including parenthesized subexpressions. The
routing-path-validation fixture contains the unchanged real PathPattern
is_valid method. It exercises nested loops, invalid segment kinds, missing or
duplicate capture names, misplaced wildcards, valid repeated literals and empty
paths. The same fixture also runs through a separately imported module.

Scalar, String and void returns inside structured scopes capture their result
before entering the shared function epilogue. Reached scope lifetimes end on
the exit edge, including enclosing loop counters and readonly record views.
Root defers run LIFO, then lexical owners drop and their regions close. Array
iteration read loans end before exit cleanup; they do not prevent a defer from
mutating the source owner after the loop has exited.

The ownership proof remains conservative: every loop back edge and conditional
join must preserve affine ownership. Structured return expressions cannot
transfer ownership; structured class results require further terminal-edge
analysis. Root defer registration after a possible structured return is rejected
until path-specific registration is implemented. Scoped owning objects, nested
exception cleanup, actor/request lifetimes and strict realtime loop certification
remain separate implementation work. No heap frame is exposed across await.

This advances the actual router but does not establish Spin server parity.
Nested owning class fields and typed HTTP actor/mailbox/await integration still
need implementation. Root String/record arenas remain bounded synchronous
invocation storage, not the eventual request/actor allocation policy.
