#include "oreslang/runtime/DeterministicGraphArena.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>
// Count C++ heap allocations only. This test's stored T does not itself
// allocate; C allocation and OS page faults require separate instrumentation.
static std::atomic<std::size_t> heap_news{0};
void* operator new(std::size_t bytes) {
  heap_news.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(bytes == 0 ? 1 : bytes)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void* operator new[](std::size_t bytes) {
  heap_news.fetch_add(1, std::memory_order_relaxed);
  if (void* p = std::malloc(bytes == 0 ? 1 : bytes)) return p;
  throw std::bad_alloc();
}
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

struct DropTracked {
  int value;
  int* drops;
  DropTracked(int v, int* d) noexcept : value(v), drops(d) {}
  DropTracked(const DropTracked&) = delete;
  DropTracked& operator=(const DropTracked&) = delete;
  ~DropTracked() noexcept { ++*drops; }
};
using Graph = oreslang::runtime::DeterministicGraphArena<DropTracked, 3, 2>;
#define VERIFY(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %d\n", __LINE__); return 1; } } while (false)
static_assert(!std::is_copy_constructible_v<Graph>, "unique arena owner");
static_assert(!std::is_move_constructible_v<Graph>, "stable arena");
static_assert(std::is_same_v<decltype(std::declval<const Graph::Lease&>().get()),
                             const DropTracked&>, "read authority is immutable");
static_assert(std::is_same_v<decltype(*std::declval<const Graph::Lease&>()),
                             const DropTracked&>, "dereference is immutable");
int main() {
  int drops = 0;
  Graph graph;
  const auto initial_news = heap_news.load(std::memory_order_relaxed);
  auto a = graph.emplace(1, &drops);
  auto b = graph.emplace(2, &drops);
  VERIFY(a.has_value() && b.has_value());
  bool nested_write = true, nested_read = true, nested_close = true;
  VERIFY(graph.with_mut(*a, [&](DropTracked& v) {
    v.value = 42;
    nested_write = graph.with_mut(*a, [](DropTracked&) {});
    nested_read = graph.borrow(*a).has_value();
    nested_close = graph.close();
  }));
  VERIFY(!nested_write && !nested_read && !nested_close);
  VERIFY(graph.connect(*a, *a));
  VERIFY(graph.connect(*a, *b));
  VERIFY(graph.connect(*b, *a));
  auto loan = graph.borrow(*a);
  VERIFY(loan.has_value() && loan->get().value == 42);
  VERIFY(!graph.with_mut(*a, [](DropTracked& v) { v.value = 99; }));
  VERIFY(!graph.erase(*a));
  VERIFY(!graph.close());
  loan.reset();
  bool threw = false;
  try {
    graph.with_mut(*a, [](DropTracked&) { throw 17; });
  } catch (int code) {
    threw = code == 17;
  }
  VERIFY(threw); // exception must release the stack-only write privilege
  VERIFY(graph.with_mut(*a, [](DropTracked& v) { v.value = 43; }));
  VERIFY(graph.erase(*a));
  VERIFY(drops == 1);
  VERIFY(!graph.borrow(*a).has_value());
  VERIFY(graph.live_edges(*b) == 0);
  auto replacement = graph.emplace(3, &drops);
  VERIFY(replacement.has_value());
  VERIFY(replacement->slot == a->slot && replacement->generation != a->generation);
  VERIFY(!graph.connect(*b, *a));
  VERIFY(graph.connect(*b, *replacement));
  VERIFY(graph.connect(*replacement, *b)); // cycle never owns either object
  VERIFY(graph.connect(*replacement, *replacement));
  Graph other;
  auto foreign = other.emplace(4, &drops);
  VERIFY(foreign.has_value());
  VERIFY(!graph.connect(*b, *foreign));
  VERIFY(!graph.borrow(*foreign).has_value());
  VERIFY(graph.close() && graph.close());
  VERIFY(!graph.borrow(*b).has_value());
  VERIFY(drops == 3);
  VERIFY(other.close());
  VERIFY(drops == 4);

  // Retirement fences future borrowers before existing async/FFI leases
  // finish; a cancellation/stop request alone cannot free the region.
  Graph draining;
  auto final_node = draining.emplace(5, &drops);
  VERIFY(final_node.has_value());
  auto outstanding = draining.borrow(*final_node);
  VERIFY(outstanding.has_value());
  draining.begin_retire();
  VERIFY(draining.retirement_requested());
  VERIFY(!draining.borrow(*final_node).has_value());
  VERIFY(!draining.with_mut(*final_node, [](DropTracked& v) { v.value = 99; }));
  VERIFY(!draining.emplace(6, &drops).has_value());
  VERIFY(!draining.close());
  VERIFY(drops == 4);
  outstanding.reset(); // physical completion/loan release, not just cancellation
  VERIFY(draining.close() && draining.closed());
  VERIFY(drops == 5);
  // Domain construction, scoped reads, exclusive writes, cyclic links and
  // region reclamation allocate no C++ heap blocks for this fixed-width T.
  VERIFY(heap_news.load(std::memory_order_relaxed) == initial_news);
  return 0;
}
