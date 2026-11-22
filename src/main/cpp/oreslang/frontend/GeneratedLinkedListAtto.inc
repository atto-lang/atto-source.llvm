// Generated from stdlib/alloc/linked_list.atto; do not edit manually.
// source Git blob SHA-1: f70cb1ac77efcef1734a526055655bfb2b1169bd
R"ATTO_LINKLIST(// LLVM-owned atto.alloc/linked_list source candidate.
// A private doubly linked index graph avoids raw pointers and cycles of
// owning heap references; removed nodes are reset and recycled explicitly.
define module linked_list as
  pub define class IntLinkedList as
    let mut Array<int> items = new Array<int>();
    let mut Array<int> forward = new Array<int>();
    let mut Array<int> backward = new Array<int>();
    let mut int first = -1;
    let mut int last = -1;
    let mut int reclaimed = -1;
    let mut int entries = 0;

    pub constructor() {}

    allocate_node(mut self)(int value): int {
      if self.reclaimed >= 0; do
        let int recycled = self.reclaimed;
        self.reclaimed = self.forward.get(recycled);
        self.items.set(recycled, value);
        self.forward.set(recycled, -1);
        self.backward.set(recycled, -1);
        return recycled;
      fi
      if self.items.size >= 1048576; do
        rt fail("linked_list: capacity exceeded");
      fi
      let int index = self.items.size;
      self.items.add(value);
      self.forward.add(-1);
      self.backward.add(-1);
      return index;
    }

    unlink_node(mut self)(int index): int {
      let int before = self.backward.get(index);
      let int after = self.forward.get(index);
      if before >= 0; do
        self.forward.set(before, after);
      else
        self.first = after;
      fi
      if after >= 0; do
        self.backward.set(after, before);
      else
        self.last = before;
      fi
      let int old = self.items.get(index);
      self.items.set(index, 0);
      self.backward.set(index, -1);
      self.forward.set(index, self.reclaimed);
      self.reclaimed = index;
      self.entries = self.entries - 1;
      return old;
    }

    pub len(): int { return self.entries; }
    pub is_empty(): bool { return self.entries == 0; }
    pub allocated_slots(): int { return self.items.size; }

    pub push_front(mut self)(int value): void {
      let int index = self.allocate_node(value);
      self.forward.set(index, self.first);
      if self.first >= 0; do
        self.backward.set(self.first, index);
      else
        self.last = index;
      fi
      self.first = index;
      self.entries = self.entries + 1;
      return;
    }

    pub push_back(mut self)(int value): void {
      let int index = self.allocate_node(value);
      self.backward.set(index, self.last);
      if self.last >= 0; do
        self.forward.set(self.last, index);
      else
        self.first = index;
      fi
      self.last = index;
      self.entries = self.entries + 1;
      return;
    }

    pub front(): Option<int> {
      if self.first == -1; do return None; fi
      return Some(self.items.get(self.first));
    }

    pub back(): Option<int> {
      if self.last == -1; do return None; fi
      return Some(self.items.get(self.last));
    }

    pub pop_front(mut self)(): Option<int> {
      if self.first == -1; do return None; fi
      return Some(self.unlink_node(self.first));
    }

    pub pop_back(mut self)(): Option<int> {
      if self.last == -1; do return None; fi
      return Some(self.unlink_node(self.last));
    }

    pub contains(int value): bool {
      let mut int node = self.first;
      for (let int i = 0; i < self.entries; i = i + 1) {
        if self.items.get(node) == value; do return true; fi
        node = self.forward.get(node);
      }
      return false;
    }

    pub remove_first(mut self)(int value): bool {
      let mut int node = self.first;
      for (let int i = 0; i < self.entries; i = i + 1) {
        if self.items.get(node) == value; do
          self.unlink_node(node);
          return true;
        fi
        node = self.forward.get(node);
      }
      return false;
    }

    pub to_array(): Array<int> {
      let mut Array<int> snapshot = new Array<int>();
      let mut int node = self.first;
      for (let int i = 0; i < self.entries; i = i + 1) {
        snapshot.add(self.items.get(node));
        node = self.forward.get(node);
      }
      return snapshot;
    }

    pub clear(mut self)(): void {
      let int count = self.entries;
      for (let int i = 0; i < count; i = i + 1) {
        self.pop_front();
      }
      return;
    }
  end

  pub fnc new_linked_list(): IntLinkedList {
    return new IntLinkedList();
  }
end
)ATTO_LINKLIST"
