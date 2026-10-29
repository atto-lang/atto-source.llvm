#include "oreslang/runtime/TrustedActorBufferDelivery.hpp"
#include <atomic>
#include <cassert>
#include <thread>
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
    using Send = TrustedActorBufferDelivery::SendStatus;
    assert(pipe.try_send(producer, {999,1}, first) == Send::UnknownRecipient);
    pipe.write(producer, first, 0, 42); // invalid target did not take ownership
    assert(pipe.try_send(producer, receiver, first) == Send::Accepted);
    assert(fails([&] {(void)pipe.try_send(producer, {999,1}, first); }));
    // A stale alias and a borrowed source are invalid ownership, not FULL.
    assert(fails([&] {(void)pipe.try_send(producer, receiver, first); }));
    auto loan = pool->borrow_read(rejected, sender, receiver);
    assert(fails([&] {(void)pipe.try_send(producer, receiver, rejected); }));
    assert(fails([&] {(void)pipe.try_send(producer, {999,1}, rejected); }));
    loan.release();
    assert(pipe.try_send(producer, receiver, rejected) == Send::MailboxFull);
    assert(fails([&] { pipe.send(producer, receiver, rejected); }));
    // Full mailbox did not revoke sender authority or copy DATA.
    pipe.write(producer, rejected, 0, 9);
    assert(fails([&] { pipe.write(producer, first, 0, 99); }));
    producer.release();
    assert(pipe.pending(receiver) == 1);

    const int out = pipe.deliver_one(receiver, [&](auto& data) {
        assert(data.read(31) == 7);
        assert(fails([&] {pipe.retire(receiver);}));
        const int result = static_cast<int>(data.read(0));
        data.release();
        assert(fails([&] {(void)data.read(0); })); // consumed payload cannot be re-read
        return result;
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
    auto terminated_source_turn = pipe.enter_turn(sender);
    auto terminated_payload = pipe.allocate(terminated_source_turn, 1);
    assert(pipe.try_send(terminated_source_turn, receiver, terminated_payload)
           == Send::RecipientStopped);
    pipe.write(terminated_source_turn, terminated_payload, 0, 21);
    pipe.drop(terminated_source_turn, terminated_payload);
    terminated_source_turn.release();
    assert(pool->used_bytes() == 0);
    assert(fails([&] {pipe.deliver_one(receiver, [](auto&) {});}));
    // Concurrent DATA admission versus terminal STOP is linearized by the
    // underlying gate: either the destination reclaims accepted DATA or the
    // source retains the rejected buffer. Neither path leaks/double-drops.
    for (std::uint64_t i=0;i<64;++i) {
        const TrustedBufferActor dying{100+i, 1};
        pipe.admit(dying, 1);
        auto setup_turn = pipe.enter_turn(sender);
        auto h = pipe.allocate(setup_turn, 2);
        pipe.write(setup_turn, h, 0, 77);
        setup_turn.release();
        std::atomic<bool> go{false}, accepted{false};
        std::thread producer([&] {
            // Physical scheduler-turn authority belongs to this carrier.
            auto source_turn = pipe.enter_turn(sender);
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            const auto outcome = pipe.try_send(source_turn, dying, h);
            assert(outcome == Send::Accepted || outcome == Send::RecipientStopped);
            accepted.store(outcome == Send::Accepted, std::memory_order_release);
        });
        std::thread stop([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            pipe.retire(dying);
        });
        go.store(true, std::memory_order_release);
        producer.join();
        stop.join();
        auto verify_turn = pipe.enter_turn(sender);
        if (accepted.load(std::memory_order_acquire)) {
            assert(fails([&] { pipe.drop(verify_turn,h); }));
        } else {
            pipe.write(verify_turn,h,0,78);
            pipe.drop(verify_turn,h);
        }
        verify_turn.release();
        assert(pool->used_bytes() == 0);
    }
    pipe.retire(sender);
    assert(pipe.close());
    assert(pipe.close());
}
