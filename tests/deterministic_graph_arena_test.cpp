#include "oreslang/runtime/DeterministicGraphArena.hpp"
#include <cstdio>
using Graph = oreslang::runtime::DeterministicGraphArena<int, 3, 2>;
#define VERIFY(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL %d\n", __LINE__); return 1; } } while (false)
int main() {
  Graph graph;
  auto a = graph.emplace(1);
  auto b = graph.emplace(2);
  VERIFY(a.has_value() && b.has_value());
  VERIFY(graph.connect(*a, *a));
  VERIFY(graph.connect(*a, *b));
  VERIFY(graph.connect(*b, *a));
  auto loan = graph.borrow(*a);
  VERIFY(loan.has_value() && loan->get() == 1);
  VERIFY(!graph.erase(*a));
  VERIFY(!graph.close());
  loan.reset();
  VERIFY(graph.erase(*a));
  VERIFY(!graph.borrow(*a).has_value());
  VERIFY(graph.live_edges(*b) == 0);
  auto replacement = graph.emplace(3);
  VERIFY(replacement.has_value());
  VERIFY(replacement->slot == a->slot && replacement->generation != a->generation);
  VERIFY(!graph.connect(*b, *a));
  VERIFY(graph.connect(*b, *replacement));
  Graph other;
  auto foreign = other.emplace(4);
  VERIFY(foreign.has_value());
  VERIFY(!graph.connect(*b, *foreign));
  VERIFY(!graph.borrow(*foreign).has_value());
  VERIFY(graph.close() && graph.close());
  VERIFY(!graph.borrow(*b).has_value());
  VERIFY(other.close());
  return 0;
}
