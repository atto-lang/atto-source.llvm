// Generated from stdlib/alloc/heap.atto; do not edit manually.
// source Git blob SHA-1: 115e09bcf2a51ddfe927b81381544e7ebec68d95
R"ATTO_HEAP(// Compiler-owned Atto alloc tier: source implements policy, not physical authority.
// Real native allocation still requires a provider and compiler-verified lifetimes.
define module heap as
// LLVM-backend-owned alloc/heap Atto source (logical allocation policy).
// Every byte count is nonnegative; -1 is an invalid/overquota sentinel.
// Physical allocation must use the explicit trusted AllocatorProviderV1,
// never a guest-made pointer or ambient malloc. This source mints no receipt.
  pub fnc heap_can_reserve(int committed, int pending, int request, int quota) -> bool {
    if committed < 0 || pending < 0 || request < 0 || quota < 0 then return false; fi
    if committed > quota then return false; fi
    if pending > quota - committed then return false; fi
    return request <= quota - committed - pending;
  }

  pub fnc heap_reserved_total(int committed, int pending, int request, int quota) -> int {
    if heap_can_reserve(committed, pending, request, quota) == false then return -1; fi
    return committed + pending + request;
  }

// The trusted provider must validate a uniquely live owned allocation receipt
// before crediting its bytes. This policy does not authenticate that receipt.
  pub fnc heap_after_release(int committed, int release_bytes) -> int {
    if committed < 0 || release_bytes < 0 || release_bytes > committed then return -1; fi
    return committed - release_bytes;
  }

  pub fnc heap_next_generation(int generation, int maximum) -> int {
    if generation < 1 || maximum < 2 || generation >= maximum then return -1; fi
    return generation + 1;
  }

  pub fnc heap_can_reclaim(bool sealed, bool drops_complete,
                           int committed, int pending, int active_turns,
                           int active_loans, int continuations, int io_leases,
                           int producer_admissions) -> bool {
    if sealed == false || drops_complete == false then return false; fi
    if committed < 0 || pending < 0 || active_turns < 0 then return false; fi
    if active_loans < 0 || continuations < 0 then return false; fi
    if io_leases < 0 || producer_admissions < 0 then return false; fi
    return committed == 0 && pending == 0 && active_turns == 0 &&
           active_loans == 0 && continuations == 0 && io_leases == 0 &&
           producer_admissions == 0;
  }

end
)ATTO_HEAP"
