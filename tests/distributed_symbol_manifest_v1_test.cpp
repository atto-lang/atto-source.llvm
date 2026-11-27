#include "DistributedSymbolManifestV1.hpp"
#include <cassert>
#include <iostream>
using namespace oreslang::distributed_symbol_v1;
template<class E, class F> void rejects(F f) {
    try { f(); } catch (const E&) { return; }
    throw std::logic_error("expected rejection");
}
int main() {
    const Uuid a{0x00,0x11,0x22,0x33,0x44,0x55,0x46,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
    const Uuid b{0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x49,0x88,0xb7,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
    const Declaration core{"fixture/core", "ready"}, app{"fixture/app", "ready"};
    const Catalog first({{core,a},{app,b}}), second({{app,b},{core,a}});
    assert(first.resolve(core) == second.resolve(core));
    assert(first.resolve(core) != second.resolve(app));
    const std::vector<std::uint8_t> wire(a.begin(), a.end());
    assert(second.receive(wire,{core}) == a);
    rejects<std::out_of_range>([&]{ second.receive(wire,{app}); });
    rejects<std::out_of_range>([&]{ second.resolve({"unknown","ready"}); });
    rejects<std::invalid_argument>([&]{ Catalog bad({{core,a},{app,a}}); });
    rejects<std::invalid_argument>([&]{ Catalog bad({{core,a},{core,b}}); });
    auto wrong = wire; wrong[6] = 0x50;
    rejects<std::invalid_argument>([&]{ decode(wrong); });
    wrong = wire; wrong[8] = 0x40;
    rejects<std::invalid_argument>([&]{ decode(wrong); });
    wrong = wire; wrong.pop_back();
    rejects<std::invalid_argument>([&]{ decode(wrong); });
    wrong = wire; wrong.push_back(0);
    rejects<std::invalid_argument>([&]{ decode(wrong); });
    wrong = wire; wrong[0] = 1;
    rejects<std::out_of_range>([&]{ second.receive(wrong,{core}); });
    rejects<std::length_error>([&]{ Catalog bad(std::vector<Binding>(4097)); });
    assert(second.size() == 2);
    std::cout << "distributed symbol host manifest checks passed\n";
}
