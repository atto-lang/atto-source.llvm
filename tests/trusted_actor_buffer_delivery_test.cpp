#include "oreslang/runtime/TrustedActorBufferDelivery.hpp"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <type_traits>

using namespace oreslang::runtime;
template <typename F> bool fails(F&& action) {
    try { action(); } catch (const std::exception&) { return true; }
    return false;
}
static_assert(!std::is_copy_constructible<TrustedActorBufferDelivery::Received>::value,
              "a delivered actor DATA value must be linear");
static_assert(!std::is_move_constructible<TrustedActorBufferDelivery::Received>::value,
              "a delivered actor DATA value cannot escape its scheduler turn");

int main() {
    const TrustedBufferActor sender{10, 1}, receiver{11, 1};
    auto pool = std::make_shared<TrustedActorBufferPool>(1, 4096, 8);
    TrustedActorBufferDelivery pipe(pool);
    pipe.admit(sender, 1);
    pipe.admit(receiver, 1);

    auto producer = pipe.enter_turn(sender);
    auto first = pipe.allocate(producer, 32);
    pipe.write(producer, first, 0, 42);
    pipe.write(producer, first, 31, 7);
    auto rejected = pipe.allocate(producer, 16);
    pipe.send(producer, receiver, first);
    assert(fails([&] { pipe.send(producer, receiver, rejected); }));
    // Full mailbox did not revoke sender authority or copy DATA.
    pipe.write(producer, rejected, 0, 9);
    assert(fails([&] { pipe.write(producer, first, 0, 99); }));
    producer.release();
    assert(pipe.pending(receiver) == 1);

    const int out = pipe.deliver_one(receiver, [&](auto& data) {
        assert(data.read(31) == 7);
        assert(fails([&] {pipe.retire(receiver);}));
        return static_cast<int>(data.read(0));
    });
    assert(out == 42);
    assert(pipe.pending(receiver) == 0);
    assert(pool->used_bytes() == 16);

    // Accepted DATA is reclaimed even if a host handler throws.
    auto next = pipe.enter_turn(sender);
    pipe.send(next, receiver, rejected);
    next.release();
    assert(fails([&] {
        pipe.deliver_one(receiver, [&](auto& data) -> void {
            assert(data.read(0) == 9);
            throw std::runtime_error("handler failure");
        });
    }));
    assert(pool->used_bytes() == 0);

    // A terminal receiver drains unread accepted DATA; sender aliases remain stale.
    auto last_turn = pipe.enter_turn(sender);
    auto last = pipe.allocate(last_turn, 4);
    pipe.send(last_turn, receiver, last);
    last_turn.release();
    pipe.retire(receiver);
    assert(pool->used_bytes() == 0);
    assert(fails([&] {pipe.deliver_one(receiver, [](auto&) {});}));
    pipe.retire(sender);
    assert(pipe.close());
    assert(pipe.close());
}
