# Actor response contracts: unary, streaming, and transport

Status: language/API proposal; the new actor modifiers and callback types below
are not compiler-admitted yet. Canonical message domains use `define match
pattern`. Java implemented that spelling in PR #684; LLVM pipeline fixtures now
use it too.

## Separate three choices

1. Execution and isolation: serial/shared/private/untrusted actor scheduling.
2. Output contract: fire-and-forget messages, unary request/reply, or a stream.
3. Transport adapter: in-process mailbox, HTTP, TCP, or WebSocket.

Transport does not choose output cardinality. Ordinary message actors have no
implicit response. Stream requests allow zero, one, or many outputs; exactly one
successful response is an explicit unary contract, never the default.

## Unary: a returned value is the response

Proposed source syntax:

```ores
define unary actor Increment as
    receive(int request): int {
        return request + 1;
    }
end
```

The runtime resolves that request's unary reply future exactly once after the
handler's successful completion. Failure/cancellation settles it with a terminal
error, rather than inventing a value. Callers use an explicit unary request API;
plain send does not silently allocate a reply future. No streaming emission is
available through this unary reply capability. A unary result may itself be a
collection: returning five values in an array is still one response.

## Stream: callbacks carry zero or more responses

Proposed source syntax:

```ores
define stream actor Expand as
    receive(int request, StreamReply<int> reply): void {
        await reply.next(request + 1);
        await reply.next(request + 2);
        await reply.next(request + 3);
        await reply.next(request + 4);
        await reply.next(request + 5);
        reply.complete();
        return;
    }
end
```

`StreamReply<T>` is a runtime-issued capability for one response stream, exposing
its callbacks through `next`, `complete`, and `fail`. It is not an arbitrary
closure that can enter another actor's state. A caller registers `on_next(T)`,
`on_complete()`, and `on_error(Error)` with an explicit stream request API. Each
of the five `next` calls dispatches one `on_next` event on the consumer's owning
scheduler lane. FIFO order is per stream; output from separate streams may
interleave. Request correlation is assigned by the runtime and not inferred from
the payload or the immediate upstream actor.

`next` returns an admission future: await provides bounded backpressure, not a
claim that the consumer has already processed the value. Completion closes this
request's output stream, not the actor or socket. It follows all admitted output
in FIFO order. Exactly one terminal event is permitted; emission after terminal
completion rejects. `fail` settles the same stream; caller cancellation fences
later emission and settles blocked producers.

Zero responses are valid: call `complete` without calling `next`. Bare `return;`
only ends handler execution. It does not emit a value or implicitly complete a
response stream. This initial contract requires completing/failing before the
handler finishes; returning with an open reply is a protocol error and fails the
stream. Deferred, long-lived responses require a separately owned reply session
with explicit lifetime/cancellation rules, not a captured callback that escapes
the actor turn.

## Ordinary message actors and pipeline callbacks

An ordinary `define actor` has `receive(Message m): void`. It may explicitly send
zero or more messages to typed actor refs or emit onto an owned output port. It
has no implicit request/reply relationship. An actor-level subscription is a
long-lived event stream, distinct from a response stream associated with one
request; a runtime-issued output-port handle carries its callbacks.

Pipeline callbacks describe routing, not RPC replies. Proposed handler shape:

```ores
receive(Message m, PipeOutput<Signal> pipe): void {
    do match m over {
        on Signal.Data as n {
            await pipe.forward(Signal.Data(n + 1));
        }
        on Signal.Result as n {
            await pipe.backward(Signal.Result(n + 1));
        }
    }
    return;
}
```

A handler may call forward/backward zero or many times per input. Forward selects
an authorized outgoing edge; backward follows an authorized ingress/reverse
route. Neither means 'respond to the original caller'. A separate StreamReply
capability is necessary for correlated responses. Source/sink roles restrict
available ports. Finishing one handler never closes the entire pipeline;
end-of-input, port completion, actor stop and graph completion are separate
lifecycle events.

## HTTP, TCP, and WebSocket

- HTTP: one request has one HTTP response, whose body may be a stream of chunks
  (including SSE events). Five callback items may be five chunks/events, not five
  independent HTTP responses. Unary adapts one result to one finite body;
  streaming adapts output to a streaming body and completes it explicitly.
- TCP: bytes have no inherent request/message boundaries. Framing and correlation
  belong to an explicit protocol adapter; a connection does not imply unary RPC.
- WebSocket: bidirectional messages form a session stream. A frame may trigger
  zero or many outgoing frames, and unsolicited events are allowed. Unary RPC
  over WebSocket requires an explicit correlation protocol.

These adapters preserve their transport's semantics while using the same actor
scheduling and output capability rules.

## Existing fifteen-actor fixture

The current LLVM bootstrap receive ABI returns i64. Source `return value` is
stored as `last_result` and observed by `expect_pipeline`, which also checks hop
counts and the terminal result. It is instrumentation/state observation in this
restricted slice, not a reply emitted to another actor. Actual messages are sent
by `self.forward` and `self.backward`. This fixture therefore does not establish
unary return semantics or general streaming callback support.

After void stream callback lowering is available, replace these value returns
with `return;` and move the fixture's state observation into a separate test-only
inspection mechanism. Keep the tested legacy ABI visible until that change is
implemented, rather than presenting unsupported syntax as an executable example.

## Required verification before stream admission

Test zero/one/five outputs for one input, independent simultaneous correlations,
FIFO per stream, bounded queues and suspended admission, consumer cancellation,
actor stop/restart, stale capabilities, duplicate terminal events, emission after
completion and thrown errors. Ensure one terminal event and exact credit release
without entering consumer guest code recursively on the producer's carrier.
Java and LLVM must share the same observable contracts.
