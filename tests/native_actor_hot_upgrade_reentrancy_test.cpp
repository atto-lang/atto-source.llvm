#include "oreslang/runtime/NativeActorHotUpgradeGate.hpp"
#include <atomic>
#include <stdexcept>
#include <string>
using Gate = oreslang::runtime::NativeActorHotUpgradeGate<std::string>;
int main() {
    std::atomic<int> cleaned{0};
    oreslang::runtime::ActorHeapIdentity id{11,22,33};
    Gate::Shape shape{Gate::Kind::trusted_private,"message","state","shared","caps"};
    Gate gate(id,shape,"first",[&]{++cleaned;});
    auto ticket = gate.freeze(id,1);
    const auto result = gate.promote(ticket,shape,"second",[&]{++cleaned;});
    if (result.revision != 2 || cleaned != 1) return 1;
    gate.close();
    return cleaned == 2 ? 0 : 2;
}
