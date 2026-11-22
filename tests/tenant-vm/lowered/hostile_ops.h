#pragma once
// Operations of hostile.wat. Input is op, a, b as i64.
enum { OP_ADD, OP_GROW, OP_PEEK, OP_PEEK_FAR, OP_POKE, OP_TRAP, OP_SPIN, OP_RECURSE, OP_INDIRECT, OP_VM_SPAWN, OP_VM_SEND, OP_VM_STOP, OP_RECURSE_IN_HOST_CALLS };
