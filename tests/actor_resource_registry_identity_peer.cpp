#include "oreslang/runtime/ActorResourceRegistry.hpp"
#include <memory>

std::unique_ptr<oreslang::runtime::ActorResourceRegistry> make_peer_registry() {
    return std::make_unique<oreslang::runtime::ActorResourceRegistry>(41, 7);
}
