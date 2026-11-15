// Host-only integration of pooled buffer mailbox and the REAL registry.
// Not a compiled .atto guest, not trusted actor scheduler authorization.
#include "oreslang/runtime/NativeFixedBufferMailbox.hpp"
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

using oreslang::runtime::ActorResourceRegistry;
using oreslang::runtime::BufferActorIdentity;
using oreslang::runtime::NativeFixedBufferMailbox;
using oreslang::runtime::NativeFixedBufferPool;
using Reclaim = ActorResourceRegistry::Reclaim;

static constexpr BufferActorIdentity id(std::uint64_t actor,
                                         std::uint64_t generation = 1,
                                         std::uint64_t tenant = 1) noexcept {
    return {tenant, actor, generation};
}

static bool invalid_target_registry() {
    auto receiver = std::make_shared<ActorResourceRegistry>(22, 2);
    try {
        NativeFixedBufferMailbox bad(1, id(22, 1), receiver);
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

int main() {
    assert(invalid_target_registry());

    {
        NativeFixedBufferPool pool(3, 64);
        auto sender = std::make_shared<ActorResourceRegistry>(11, 1);
        auto receiver = std::make_shared<ActorResourceRegistry>(22, 1);
        NativeFixedBufferMailbox mailbox(2, id(22), receiver);
        auto first = pool.try_acquire(id(11));
        auto second = pool.try_acquire(id(11));
        assert(first && second);
        assert(!mailbox.try_offer(pool, *first, id(11))); // cannot bypass bound registry
        assert(!mailbox.try_offer_authenticated(pool, *first, id(11, 2), *sender));
        assert(!mailbox.try_offer_authenticated(pool, *first, id(11, 1, 2), *sender));
        assert(first->with_mutable_bytes(id(11), [](auto s) { s[0] = 71; }));
        assert(mailbox.try_offer_authenticated(pool, *first, id(11), *sender));
        assert(mailbox.try_offer_authenticated(pool, *second, id(11), *sender));
        assert(!first->release() && !second->release());
        assert(receiver->phase() == ActorResourceRegistry::Phase::live);

        // The source's in-flight handoff ticket is released only after
        // publication. Receiver loan stays outstanding until physical ACK.
        sender->retire();
        assert(sender->try_reclaim().status == Reclaim::complete);
        receiver->retire();
        assert(receiver->try_reclaim().status == Reclaim::busy);
        auto a = mailbox.try_receive();
        assert(a && !a->lease().with_read_only_bytes(id(11), [](auto) {}));
        assert(a->lease().with_read_only_bytes(id(22), [](auto s) {
            assert(s[0] == 71);
        }));
        assert(receiver->try_reclaim().status == Reclaim::busy);
        assert(a->acknowledge_released());
        assert(!a->acknowledge_released());
        assert(receiver->try_reclaim().status == Reclaim::busy);
        auto b = mailbox.try_receive();
        assert(b && b->acknowledge_released());
        assert(mailbox.charged() == 0 && mailbox.retire());
        assert(receiver->try_reclaim().status == Reclaim::complete);
        assert(pool.in_use() == 0);
    }

    {
        // Queue exhaustion cannot consume sender's lease and cannot leave
        // the extra target loan or source handoff ticket outstanding.
        NativeFixedBufferPool pool(3, 16);
        auto source = std::make_shared<ActorResourceRegistry>(31, 1);
        auto dest = std::make_shared<ActorResourceRegistry>(32, 1);
        NativeFixedBufferMailbox mailbox(1, id(32), dest);
        auto held = pool.try_acquire(id(31));
        auto rejected = pool.try_acquire(id(31));
        assert(held && rejected);
        assert(mailbox.try_offer_authenticated(pool, *held, id(31), *source));
        assert(!mailbox.try_offer_authenticated(pool, *rejected, id(31), *source));
        assert(rejected->with_read_only_bytes(id(31), [](auto) {}));
        source->retire();
        assert(source->try_reclaim().status == Reclaim::complete);
        dest->retire();
        assert(dest->try_reclaim().status == Reclaim::busy);
        auto msg = mailbox.try_receive();
        assert(msg && msg->acknowledge_released());
        assert(dest->try_reclaim().status == Reclaim::complete);
        assert(rejected->release());
    }

    {
        // Retired receiver blocks new loans. Failure must not consume the
        // sender and a transient source handoff must be fully reconciled.
        NativeFixedBufferPool pool(1, 16);
        auto source = std::make_shared<ActorResourceRegistry>(41, 1);
        auto dest = std::make_shared<ActorResourceRegistry>(42, 1);
        NativeFixedBufferMailbox mailbox(1, id(42), dest);
        dest->retire();
        auto value = pool.try_acquire(id(41));
        assert(value);
        assert(!mailbox.try_offer_authenticated(pool, *value, id(41), *source));
        assert(value->with_read_only_bytes(id(41), [](auto) {}));
        source->retire();
        assert(source->try_reclaim().status == Reclaim::complete);
        assert(dest->try_reclaim().status == Reclaim::complete);
        assert(value->release());
    }

    {
        // Unacknowledged dropped delivery is not a physical release receipt:
        // the registry stays BUSY and credit remains charged, fail-closed.
        NativeFixedBufferPool pool(1, 16);
        auto source = std::make_shared<ActorResourceRegistry>(51, 1);
        auto dest = std::make_shared<ActorResourceRegistry>(52, 1);
        NativeFixedBufferMailbox mailbox(1, id(52), dest);
        auto value = pool.try_acquire(id(51));
        assert(value && mailbox.try_offer_authenticated(pool, *value, id(51), *source));
        { auto lost = mailbox.try_receive(); assert(lost); }
        source->retire();
        dest->retire();
        assert(source->try_reclaim().status == Reclaim::complete);
        assert(dest->try_reclaim().status == Reclaim::busy);
        assert(mailbox.charged() == 1 && !mailbox.retire());
        // No forged teardown ACK or made-up reclaim. This specific test
        // intentionally leaves the denied registry in a blocked state.
    }
}
