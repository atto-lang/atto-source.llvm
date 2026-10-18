#include "oreslang/runtime/PersistentActorMap.hpp"
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::runtime;

static PureActorTransition add(const PersistentActorMap& state, std::int64_t delta) {
    const auto* prior = state.get("count");
    std::int64_t value = prior
        ? std::get<std::int64_t>(prior->storage()) : 0;
    return PureActorTransition(
        state.with("count", value + delta),
        std::vector<PureActorCommand>{{7, value + delta}});
}

int main() {
    auto empty = PersistentActorMap::empty();
    auto aa = empty.with("Aa", 1);
    auto both = aa.with("BB", 2);
    assert(aa.size() == 1 && both.size() == 2);
    assert(std::get<std::int64_t>(both.get("Aa")->storage()) == 1);
    auto deleted = both.without("Aa");
    assert(!deleted.contains("Aa") && both.contains("Aa"));

    auto nested = std::make_shared<const PersistentActorMap>(both);
    auto parent = empty.with("nested", PersistentActorMap::Value(nested));
    auto same = parent.with("nested", PersistentActorMap::Value(nested));
    assert(same.size() == 1);
    assert(std::get<PersistentActorMap::MapRef>(
        same.get("nested")->storage()) == nested);

    PureActorReducer reducer = &add;
    auto transition = reducer(empty, 3);
    assert(std::get<std::int64_t>(
        transition.state.get("count")->storage()) == 3);
    assert(transition.commands.size() == 1);
    assert(transition.commands[0].target == 7);

    bool bad_target = false;
    try { PureActorTransition invalid(empty, {{0, 1}}); }
    catch (const std::invalid_argument&) { bad_target = true; }
    assert(bad_target);

    PersistentActorMap flood = empty;
    for (int n = 0; n < 64; ++n) {
        std::string key;
        for (int i = 0; i < 7; ++i)
            key += ((n >> i) & 1) ? "BB" : "Aa";
        flood = flood.with(key, n);
    }
    bool collision_denied = false;
    try { (void)flood.with("AaAaAaAaAaAaBB", 65); }
    catch (const std::length_error&) { collision_denied = true; }
    assert(collision_denied);
}
