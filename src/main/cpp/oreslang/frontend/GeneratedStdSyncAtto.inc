// Generated from stdlib/std/sync.atto; do not edit manually.
// source Git blob SHA-1: 8805fd7ac43200102e08c95a367419e6e201c1d6
R"ATTO_STD_SYNC(// LLVM-owned native Atto std/sync admission/overflow policies.
// Not runnable synchronization primitives: the runtime must implement atomic
// lock ownership, fair waiter queues, cancellation, wakeups and drop fencing.
define module sync as
  pub fnc mutex_take_admissible(bool held, bool closed, int queued): bool {
    return !held && !closed && queued == 0;
  }

  pub fnc mutex_enqueue_admissible(bool closed, int queued, int capacity): bool {
    return !closed && queued >= 0 && capacity >= 1 && queued < capacity;
  }

  pub fnc read_take_admissible(int readers, bool writer, int pending_writers,
                               int reader_limit): bool {
    if writer || pending_writers != 0 || readers < 0 then return false; fi
    return reader_limit >= 1 && readers < reader_limit;
  }

  pub fnc write_take_admissible(int readers, bool writer): bool {
    return readers == 0 && !writer;
  }

  pub fnc sem_take_admissible(int available, int requested,
                              int capacity, bool closed): bool {
    if closed || capacity < 1 || available < 0 || available > capacity then return false; fi
    return requested >= 1 && requested <= available;
  }

  pub fnc sem_return_admissible(int available, int released,
                                int capacity, bool closed): bool {
    // Closing the semaphore stops new acquisitions, not release of owned permits.
    if capacity < 1 || available < 0 || available > capacity then return false; fi
    return released >= 1 && released <= capacity - available;
  }

  pub fnc barrier_last_arrival(int arrived, int parties, bool broken): bool {
    if broken || parties < 1 || parties > 65536 then return false; fi
    return arrived == parties - 1;
  }

  pub fnc barrier_arrival_admissible(int arrived, int parties, bool broken): bool {
    if broken || parties < 1 || parties > 65536 then return false; fi
    return arrived >= 0 && arrived < parties;
  }

  // These checks are NOT ownership verification. The trusted scheduler
  // must authenticate both owner and generation from a sealed receipt.
  // A consumed receipt must never release the lock twice.
  pub fnc mutex_release_receipt_admissible(bool held, int active_owner,
                                           int caller, int active_generation,
                                           int receipt_generation, bool consumed): bool {
    if !held || consumed || active_owner <= 0 then return false; fi
    if caller != active_owner then return false; fi
    if active_generation < 0 || receipt_generation < 0 then return false; fi
    return active_generation == receipt_generation;
  }

  // Writer-preference upgrade; exact ownership of the single read lease and
  // non-reentrant atomic transition remain runtime proof obligations.
  pub fnc rw_upgrade_admissible(int readers, bool writer_held,
                                int queued_writers, bool caller_has_read_lease): bool {
    if !caller_has_read_lease || writer_held then return false; fi
    return readers == 1 && queued_writers == 0;
  }

  pub fnc next_epoch_admissible(int current, int proposed): bool {
    if current < 0 || current >= 2147483647 then return false; fi
    return proposed == current + 1;
  }
end
)ATTO_STD_SYNC"
