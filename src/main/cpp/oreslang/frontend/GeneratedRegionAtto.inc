// Generated from stdlib/alloc/region.atto; do not edit manually.
// source Git blob SHA-1: bd47dfe3ee758e4b52b297944870c11406928b25
R"ATTO_REGION(// Compiler-owned Atto alloc tier: source implements policy, not physical authority.
// Real native allocation still requires a provider and compiler-verified lifetimes.
define module region as
// LLVM-backend-owned alloc/region lifecycle policy in Atto source.
// All counters must be authenticated and compiler/runtime-maintained.
// Region policy never mints an ownership loan or physically frees storage.
  pub fnc region_owner_matches(int owner, int incarnation,
                               int required_owner, int required_incarnation) -> bool {
    if owner <= 0 || incarnation <= 0 then return false; fi
    return owner == required_owner && incarnation == required_incarnation;
  }

  pub fnc region_quiescent(int active_turns, int loans, int continuations,
                           int io_leases, int producer_admissions, int children) -> bool {
    if active_turns < 0 || loans < 0 || continuations < 0 then return false; fi
    if io_leases < 0 || producer_admissions < 0 || children < 0 then return false; fi
    return active_turns == 0 && loans == 0 && continuations == 0 &&
           io_leases == 0 && producer_admissions == 0 && children == 0;
  }

  pub fnc region_can_destroy(bool admission_sealed, bool drops_complete,
                             int active_turns, int loans, int continuations,
                             int io_leases, int producer_admissions, int children) -> bool {
    if admission_sealed == false || drops_complete == false then return false; fi
    return region_quiescent(active_turns, loans, continuations, io_leases,
                            producer_admissions, children);
  }

  pub fnc region_next_epoch(int epoch, int maximum) -> int {
    if epoch < 1 || maximum < 2 || epoch >= maximum then return -1; fi
    return epoch + 1;
  }

end
)ATTO_REGION"
