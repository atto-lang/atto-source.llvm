// Generated from stdlib/alloc/slab.atto; do not edit manually.
// source Git blob SHA-1: 68cd3a7ea25bc523b52a6c00b71337b3602a4625
R"ATTO_SLAB(// AttoLang LLVM alloc/slab: bounded scalar slab, implemented in .atto.
// Handles are logical metadata, NOT transferable authority or raw pointers.
// This first version stores int values. Typed/owned payload storage requires
// compiler-proven move/drop/borrow semantics before generalizing to Slab<T>.
define module slab as
  // Each slab creates a fresh reference-identity token. Public construction of
  // another token does not reveal or reproduce the private instance token.
  pub define class OwnerToken as
    pub constructor() { return; }
  end

  pub define class Handle as
    let OwnerToken owner_token;
    pub val int domain;
    pub val int slot;
    pub val int generation;

    pub constructor(OwnerToken owner_token, int domain, int slot, int generation) {
      self.owner_token = owner_token;
      self.domain = domain;
      self.slot = slot;
      self.generation = generation;
    }

    // Identity must be checked before numeric slot and generation checks.
    pub matches_owner(OwnerToken expected): bool {
      return self.owner_token is expected;
    }
  end

  pub define class Slab as
    let OwnerToken owner_token = new OwnerToken();
    let int domain;
    let int limit;
    let mut Array<int> values = new Array<int>();
    let mut Array<int> generations = new Array<int>();
    let mut Array<bool> occupied = new Array<bool>();
    let mut Array<int> next_free = new Array<int>();
    let mut int free_head = -1;
    let mut int live_count = 0;
    let mut bool closed = false;

    pub constructor(int domain, int limit) {
      if domain < 1 || limit < 1 || limit > 1048576; do
        rt fail("slab: invalid domain or bounded capacity");
      fi
      self.domain = domain;
      self.limit = limit;
    }

    // No mutation on failure. Reuse always advances the generation at removal.
    pub insert(mut self)(int value): Result<Handle, String> {
      if self.closed; do return Err("Closed"); fi
      if self.free_head >= 0; do
        let int index = self.free_head;
        self.free_head = self.next_free.get(index);
        self.next_free.set(index, -1);
        self.values.set(index, value);
        self.occupied.set(index, true);
        self.live_count = self.live_count + 1;
        return Ok(new Handle(self.owner_token, self.domain, index, self.generations.get(index)));
      fi
      if self.values.size >= self.limit; do return Err("Full"); fi
      let int index = self.values.size;
      self.values.add(value);
      self.generations.add(1);
      self.occupied.add(true);
      self.next_free.add(-1);
      self.live_count = self.live_count + 1;
      return Ok(new Handle(self.owner_token, self.domain, index, 1));
    }

    pub contains(Handle handle): bool {
      if self.closed || !handle.matches_owner(self.owner_token) ||
         handle.domain != self.domain ||
         handle.slot < 0 || handle.slot >= self.values.size ||
         handle.generation < 1; do
        return false;
      fi
      return self.occupied.get(handle.slot) &&
             self.generations.get(handle.slot) == handle.generation;
    }

    pub read(Handle handle): Result<int, String> {
      if !self.contains(handle); do return Err("InvalidHandle"); fi
      return Ok(self.values.get(handle.slot));
    }

    pub replace(mut self)(Handle handle, int value): bool {
      if !self.contains(handle); do return false; fi
      self.values.set(handle.slot, value);
      return true;
    }

    // Removal invalidates the old handle, including when the same slot is reused.
    // A slot at generation INT_MAX is quarantined forever (no ABA wrap).
    pub remove(mut self)(Handle handle): Result<int, String> {
      if !self.contains(handle); do return Err("InvalidHandle"); fi
      let int index = handle.slot;
      let int previous = self.values.get(index);
      self.values.set(index, 0);
      self.occupied.set(index, false);
      self.live_count = self.live_count - 1;
      let int generation = self.generations.get(index);
      if generation < 2147483647; do
        self.generations.set(index, generation + 1);
        self.next_free.set(index, self.free_head);
        self.free_head = index;
      fi
      return Ok(previous);
    }

    // Invalidates every live handle; never resets generations to 1.
    pub clear(mut self)(): void {
      self.free_head = -1;
      for (let int i = 0; i < self.values.size; i = i + 1) {
        if self.occupied.get(i); do
          self.occupied.set(i, false);
          self.values.set(i, 0);
          if self.generations.get(i) < 2147483647; do
            self.generations.set(i, self.generations.get(i) + 1);
          fi
        fi
        if self.generations.get(i) < 2147483647; do
          self.next_free.set(i, self.free_head);
          self.free_head = i;
        else
          self.next_free.set(i, -1);
        fi
      }
      self.live_count = 0;
    }

    pub close(mut self)(): void {
      if self.closed; do return; fi
      self.clear();
      self.closed = true;
    }

    pub size(): int { return self.live_count; }
    pub capacity(): int { return self.limit; }
    pub slots_created(): int { return self.values.size; }
    pub is_closed(): bool { return self.closed; }
  end

  pub fnc new_slab(int domain, int capacity): Slab {
    return new Slab(domain, capacity);
  }
end
)ATTO_SLAB"
