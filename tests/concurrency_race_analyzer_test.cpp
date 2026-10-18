#include "oreslang/runtime/ConcurrencyRaceAnalyzer.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

using oreslang::runtime::ConcurrencyRaceAnalyzer;

int main() {
  {
    ConcurrencyRaceAnalyzer model;
    model.write("a", "counter");
    model.write("b", "counter");
    model.read("c", "counter");
    const auto races = model.findings();
    assert(races.size() == 2);
    assert(races[0].conflict == "write-write");
    assert(races[1].conflict == "write-read");
    assert(model.event_count() == 3);
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.read("a", "value");
    model.write("a", "value");
    model.read("a", "value");
    assert(model.findings().empty());
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.read("a", "counter");
    model.read("b", "counter");
    assert(model.findings().empty());
    model.write("c", "counter");
    assert(model.findings().size() == 2);
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.write("a", "counter");
    model.send("a", "m1");
    model.receive("b", "m1");
    model.write("b", "counter");
    assert(model.findings().empty());
    bool rejected = false;
    try {
      model.receive("c", "m1");
    } catch (const std::logic_error&) {
      rejected = true;
    }
    assert(rejected);
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.write("a", "counter");
    model.release("a", "lock");
    model.acquire("b", "lock");
    model.read("b", "counter");
    model.write("b", "counter");
    assert(model.findings().empty());
  }
  return 0;
}
