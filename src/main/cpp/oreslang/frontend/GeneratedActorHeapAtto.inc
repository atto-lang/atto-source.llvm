// Generated from stdlib/alloc/actor_heap.atto; do not edit manually.
// source Git blob SHA-1: 0bd2b5f7f097116b84ed56816d400454afd39bb6
R"ATTO_AHEAP(// AttoLang LLVM alloc/actor_heap: bounded actor-local *logical* word allocator.
// Physical Array allocations remain backend-managed. This is NOT evidence
// of off-heap allocation, zero GC, or enforced actor isolation. The compiler
// must bind this object to an actor incarnation, prevent cross-actor access,
// run deterministic cleanup, and fence outstanding native loans/turns.
// Handles are metadata scoped to the owning heap object, NOT authorization.
define module actor_heap as
  // Per-heap logical identity: numeric incarnations alone can collide.
  // This does not replace backend-authenticated actor and carrier identity.
  pub define class OwnerToken as
    pub constructor() { return; }
  end

  pub define class HeapHandle as
    let OwnerToken owner_token;
    pub val int incarnation;
    pub val int block;
    pub val int generation;
    pub constructor(OwnerToken owner_token, int incarnation, int block, int generation) {
      self.owner_token = owner_token;
      self.incarnation = incarnation;
      self.block = block;
      self.generation = generation;
    }

    pub matches_owner(OwnerToken expected): bool {
      return self.owner_token is expected;
    }
  end

  // Metadata is internal; callers must not mutate block generation or extents.
  define class Block as
    pub val int start;
    pub val int extent;
    let mut int requested;
    let mut int generation = 1;
    let mut bool active = true;

    pub constructor(int start, int extent) {
      self.start = start;
      self.extent = extent;
      self.requested = extent;
    }

    pub activate(mut self)(int requested): void {
      self.requested = requested;
      self.active = true;
    }

    pub retire(mut self)(): void {
      self.requested = 0;
      self.active = false;
      if self.generation < 2147483647; do
        self.generation = self.generation + 1;
      fi
    }

    pub reusable(int words): bool {
      return !self.active && self.generation < 2147483647 &&
             self.extent >= words;
    }
    pub is_active(): bool { return self.active; }
    pub token(): int { return self.generation; }
    pub length(): int { return self.requested; }
  end

  pub define class ActorHeap as
    let OwnerToken owner_token = new OwnerToken();
    let int incarnation;
    let int capacity_words;
    let int max_blocks;
    let mut Array<int> words = new Array<int>();
    let mut Array<Block> blocks = new Array<Block>();
    let mut int tail = 0;
    let mut int charged_words = 0;
    let mut int live_blocks = 0;
    let mut bool closed = false;

    pub constructor(int incarnation, int capacity_words, int max_blocks) {
      if incarnation < 1 || capacity_words < 1 ||
         capacity_words > 1048576 || max_blocks < 1 ||
         max_blocks > 1048576; do
        rt fail("actor_heap: invalid incarnation or quota");
      fi
      self.incarnation = incarnation;
      self.capacity_words = capacity_words;
      self.max_blocks = max_blocks;
      for (let int i = 0; i < capacity_words; i = i + 1) {
        self.words.add(0);
      }
    }

    // First-fit in released spans, then bounded bump allocation.
    // Reusing a larger span charges the full physical extent.
    pub allocate(mut self)(int count): Result<HeapHandle, String> {
      if self.closed; do return Err("Closed"); fi
      if count < 1 || count > self.capacity_words; do
        return Err("InvalidSize");
      fi
      for (let int i = 0; i < self.blocks.size; i = i + 1) {
        let mut Block block = self.blocks.get(i);
        if block.reusable(count); do
          block.activate(count);
          self.charged_words = self.charged_words + block.extent;
          self.live_blocks = self.live_blocks + 1;
          return Ok(new HeapHandle(self.owner_token, self.incarnation, i, block.token()));
        fi
      }
      if self.blocks.size >= self.max_blocks ||
         count > self.capacity_words - self.tail; do
        return Err("NoContiguousSpace");
      fi
      let int start = self.tail;
      self.tail = self.tail + count;
      self.blocks.add(new Block(start, count));
      self.charged_words = self.charged_words + count;
      self.live_blocks = self.live_blocks + 1;
      return Ok(new HeapHandle(self.owner_token, self.incarnation, self.blocks.size - 1, 1));
    }

    pub contains(HeapHandle handle): bool {
      if self.closed || !handle.matches_owner(self.owner_token) ||
         handle.incarnation != self.incarnation ||
         handle.block < 0 || handle.block >= self.blocks.size ||
         handle.generation < 1; do
        return false;
      fi
      let Block block = self.blocks.get(handle.block);
      return block.is_active() && block.token() == handle.generation;
    }

    pub read(HeapHandle handle, int index): Result<int, String> {
      if !self.contains(handle); do return Err("InvalidHandle"); fi
      let Block block = self.blocks.get(handle.block);
      if index < 0 || index >= block.length(); do
        return Err("OutOfBounds");
      fi
      return Ok(self.words.get(block.start + index));
    }

    pub write(mut self)(HeapHandle handle, int index, int value): bool {
      if !self.contains(handle); do return false; fi
      let Block block = self.blocks.get(handle.block);
      if index < 0 || index >= block.length(); do return false; fi
      self.words.set(block.start + index, value);
      return true;
    }

    // Zeroes all words in the physical extent, refunds once and invalidates
    // the handle. Generation exhaustion permanently quarantines the extent.
    pub release(mut self)(HeapHandle handle): bool {
      if !self.contains(handle); do return false; fi
      let mut Block block = self.blocks.get(handle.block);
      for (let int i = 0; i < block.extent; i = i + 1) {
        self.words.set(block.start + i, 0);
      }
      self.charged_words = self.charged_words - block.extent;
      self.live_blocks = self.live_blocks - 1;
      block.retire();
      return true;
    }

    // Logical retirement; runtime must separately drain leases, close native
    // allocations and invoke destructors before freeing physical actor memory.
    pub close(mut self)(): void {
      if self.closed; do return; fi
      for (let int i = 0; i < self.words.size; i = i + 1) {
        self.words.set(i, 0);
      }
      for (let int i = 0; i < self.blocks.size; i = i + 1) {
        let mut Block block = self.blocks.get(i);
        if block.is_active(); do block.retire(); fi
      }
      self.charged_words = 0;
      self.live_blocks = 0;
      self.closed = true;
    }

    pub in_use_words(): int { return self.charged_words; }
    pub active_blocks(): int { return self.live_blocks; }
    pub capacity(): int { return self.capacity_words; }
    pub allocated_high_water(): int { return self.tail; }
    pub is_closed(): bool { return self.closed; }
    pub epoch(): int { return self.incarnation; }
  end

  pub fnc new_actor_heap(int incarnation, int capacity_words,
                            int max_blocks): ActorHeap {
    return new ActorHeap(incarnation, capacity_words, max_blocks);
  }
end
)ATTO_AHEAP"
