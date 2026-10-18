#include "oreslang/runtime/ActorMessageBoundary.hpp"
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

using namespace oreslang::runtime;

static_assert(actor_sendable_v<std::int64_t>);
static_assert(actor_sendable_v<PersistentActorMap::MapRef>);
static_assert(!wire_serializable_v<
    WireFormat::MessagePackV2, PersistentActorMap::MapRef>);
static_assert(wire_serializable_v<WireFormat::MessagePackV2, std::int64_t>);
static_assert(!actor_sendable_v<ActorRefAuthority>);
static_assert(!actor_sendable_v<SupervisorControlEvent>);
static_assert(!wire_serializable_v<
    WireFormat::MessagePackV2, ActorRefAuthority>);
static_assert(!std::is_constructible_v<ActorDataValue, ActorRefAuthority>);
static_assert(!std::is_constructible_v<ActorDataValue, SupervisorControlEvent>);

int main() {
    auto ref = RuntimeAuthorityFactory::actor_ref(9, 3);
    assert(ref.actor() == 9 && ref.generation() == 3);

    auto map = std::make_shared<const PersistentActorMap>(
        PersistentActorMap::empty().with("x", 1));
    ActorDataLane data(1, 32);
    assert(data.offer(ActorDataValue(map), 16));
    assert(!data.offer(ActorDataValue(std::int64_t{2}), 16));
    auto delivery = data.try_receive();
    assert(delivery && data.used_frames() == 1);
    assert(std::get<PersistentActorMap::MapRef>(delivery->value()) == map);
    assert(delivery->acknowledge());
    assert(!delivery->acknowledge());
    assert(data.used_frames() == 0);

    SupervisorControlLane control(1);
    auto panic = SupervisorControlFactory::terminal(
        9, 3, ControlKind::Panic);
    assert(control.publish(std::move(panic)));
    auto event = control.poll();
    assert(event && event->actor() == 9 &&
           event->generation() == 3 &&
           event->kind() == ControlKind::Panic);
}
