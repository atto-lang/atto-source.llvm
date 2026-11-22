R"ATTO_CORE4(// LLVM-owned Atto source; independent of other compiler repository implementations.
// Borrow-state transition POLICY only: not a substitute for compiler borrow checking.
// 0 = unborrowed, -1 = exclusive borrow, N>0 = N shared borrows.
// State belongs inside a compiler/runtime-owned Cell<T> or RefCell<T> capability;
// exposing or calling these helpers alone does not grant memory access.
// A real Cell<T> implementation MUST enforce non-escaping scoped references,
// actor ownership, synchronization, generation checks, and no awaits while held.
define module cell as
  pub fnc may_borrow_shared(int state): bool {
    return state >= 0 && state < 9223372036854775807;
  }
  pub fnc may_borrow_exclusive(int state): bool {
    return state == 0;
  }
  pub fnc may_destroy(int state): bool {
    return state == 0;
  }
  // These are conservative *admission decisions*, not loan objects.
  // Actual rt borrow/take lifetime and actor ownership must be proven
  // by this backend's compiler, before any program can use a Cell<T>.
  pub fnc is_valid_state(int state): bool {
    return state >= -1;
  }
  pub fnc may_owner_read(int state): bool {
    return state >= 0;
  }
  pub fnc may_owner_write(int state): bool {
    return state == 0;
  }
  pub fnc may_move_owner(int state): bool {
    return state == 0;
  }
  pub fnc may_suspend_unpinned(int state): bool {
    return state == 0;
  }
  pub fnc borrow_shared_state(int state): Result<int, String> {
    if state < 0; do
      return Err("cell: exclusive borrow is active");
    fi
    if state == 9223372036854775807; do
      return Err("cell: shared borrow count exhausted");
    fi
    return Ok(state + 1);
  }
  pub fnc borrow_exclusive_state(int state): Result<int, String> {
    if state != 0; do
      return Err("cell: another borrow is active");
    fi
    return Ok(-1);
  }
  pub fnc release_shared_state(int state): Result<int, String> {
    if state <= 0; do
      return Err("cell: no shared borrow to release");
    fi
    return Ok(state - 1);
  }
  pub fnc release_exclusive_state(int state): Result<int, String> {
    if state != -1; do
      return Err("cell: no exclusive borrow to release");
    fi
    return Ok(0);
  }
end
)ATTO_CORE4"
