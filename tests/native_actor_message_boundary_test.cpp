#include "oreslang/runtime/ActorMessageBoundary.hpp"
#include <cassert>
#include <cstdint>
#include <memory>
#include <limits>
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
    ActorDataValue accepted(map);
    assert(data.offer(accepted, 16));
    ActorDataValue rejected(std::string("sender-owned"));
    assert(!data.offer(rejected, 16));
    assert(std::get<std::string>(rejected) == "sender-owned");
    bool close_denied = false;
    try { data.close(); }
    catch (const std::logic_error&) { close_denied = true; }
    assert(close_denied);
    auto delivery = data.try_receive();
    assert(delivery && data.used_frames() == 1);
    assert(std::get<PersistentActorMap::MapRef>(delivery->value()) == map);
    assert(delivery->acknowledge());
    assert(!delivery->acknowledge());
    assert(data.used_frames() == 0);
    data.close();

    bool nonfinite_denied = false;
    try {
        ActorDataLane bad(1, 32);
        (void)bad.offer(ActorDataValue(
            std::numeric_limits<double>::infinity()), 8);
    } catch (const std::invalid_argument&) {
        nonfinite_denied = true;
    }
    assert(nonfinite_denied);

    bool utf8_denied = false;
    try {
        ActorDataLane bad(1, 32);
        (void)bad.offer(ActorDataValue(
            std::string("\xC0\xAF", 2)), 8);
    } catch (const std::invalid_argument&) {
        utf8_denied = true;
    }
    assert(utf8_denied);

    bool null_root_denied = false;
    try {
        ActorDataLane bad(1, 32);
        (void)bad.offer(ActorDataValue(PersistentActorMap::MapRef{}), 8);
    } catch (const std::invalid_argument&) {
        null_root_denied = true;
    }
    assert(null_root_denied);

    SupervisorControlLane control(9, 3, 1);
    auto panic = SupervisorControlFactory::terminal(
        9, 3, ControlKind::Panic);
    assert(control.publish(std::move(panic)));
    bool replay_denied = false;
    try { (void)control.publish(std::move(panic)); }
    catch (const std::logic_error&) { replay_denied = true; }
    assert(replay_denied);
    auto event = control.poll();
    assert(event && event->actor() == 9 &&
           event->generation() == 3 &&
           event->kind() == ControlKind::Panic);

    auto stale = SupervisorControlFactory::terminal(
        9, 2, ControlKind::Cancel);
    bool stale_denied = false;
    try { (void)control.publish(std::move(stale)); }
    catch (const std::logic_error&) { stale_denied = true; }
    assert(stale_denied);
}
