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
    assert(races.size() == 3);
    assert(races[0].conflict == "write-write");
    assert(races[1].conflict == "write-read");
    assert(races[2].conflict == "write-read");
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
    model.acquire("a", "lock");
    model.write("a", "counter");
    model.release("a", "lock");
    model.acquire("b", "lock");
    model.read("b", "counter");
    model.write("b", "counter");
    assert(model.findings().empty());
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.write("a", "counter");
    model.write("b", "counter");
    model.send("b", "b-to-c");
    model.receive("c", "b-to-c");
    model.read("c", "counter");
    assert(model.conflict_count() == 2);
    const auto races = model.findings();
    assert(races[1].prior_participant == "a");
    assert(races[1].current_participant == "c");
  }
  {
    ConcurrencyRaceAnalyzer model;
    bool rejected = false;
    try {
      model.release("a", "lock");
    } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    model.acquire("a", "lock");
    rejected = false;
    try {
      model.acquire("b", "lock");
    } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    rejected = false;
    try {
      model.release("b", "lock");
    } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    model.release("a", "lock");
    model.acquire("b", "lock");
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.send("a", "once");
    model.receive("b", "once");
    bool rejected = false;
    try {
      model.send("b", "once");
    } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.acquire("a", "critical-state");
    model.acquire("a", "critical-state");
    model.write("a", "actor-state");
    model.release("a", "critical-state");
    bool rejected = false;
    try {
      model.acquire("b", "critical-state");
    } catch (const std::logic_error&) { rejected = true; }
    assert(rejected);
    model.release("a", "critical-state");
    model.acquire("b", "critical-state");
    model.read("b", "actor-state");
    assert(model.conflict_count() == 0);
  }
  {
    ConcurrencyRaceAnalyzer model;
    model.acquire("a", "critical-state");
    model.acquire("a", "critical-state");
    model.write("a", "actor-state");
    model.release("a", "critical-state");
    model.read("b", "actor-state");
    assert(model.conflict_count() == 1);
    model.release("a", "critical-state");
  }
  return 0;
}
