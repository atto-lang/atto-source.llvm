#include "oreslang/runtime/SharedImmutablePayload.hpp"
#include <array>
#include <atomic>
#if defined(NDEBUG)
#undef NDEBUG // Keep the native verification oracle active in Release builds.
#endif
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>

using oreslang::runtime::SharedImmutablePayload;
using oreslang::runtime::SharedPayloadInbox;
using Result = SharedPayloadInbox::SendResult;

struct BorrowedSpanReturn {
    std::span<const std::uint8_t> operator()(
        std::span<const std::uint8_t> bytes) const { return bytes; }
};
struct VoidSpanConsumer {
    void operator()(std::span<const std::uint8_t>) const { }
};
template<class Fn>
concept AllowedSpanCallback = requires(const SharedImmutablePayload& value, Fn callback) {
    value.with_read_only_span(callback);
};
static_assert(!AllowedSpanCallback<BorrowedSpanReturn>,
              "payload span must not escape directly through a callback return");
static_assert(AllowedSpanCallback<VoidSpanConsumer>,
              "a synchronous void consumer must remain admissible");

int main() {
    static_assert(std::is_same_v<decltype(std::declval<const SharedImmutablePayload&>()
                         .read_byte(0)), std::optional<std::uint8_t>>);
    const std::size_t mib = 1024 * 1024;
    std::vector<std::uint8_t> owned(mib, 42);
    const auto before = owned.data();
    auto root = SharedImmutablePayload::publish(std::move(owned));
    root.with_read_only_span([&](auto bytes) {
        assert(bytes.data() == before); // Publication MOVES, doesn't copy.
        assert(bytes.size() == mib);
    });
    auto slice = root.slice(5, 99);
    assert(slice && slice->same_backing(root));
    assert(slice->read_byte(0) == 42 && slice->read_byte(98) == 42);
    assert(!slice->read_byte(99));
    assert(!slice->slice(99, 1));
    assert(!slice->slice(std::numeric_limits<std::size_t>::max(), 1));
    assert(!root.slice(mib, 1));
    assert(root.slice(mib, 0));
    assert(!SharedImmutablePayload{}.read_byte(0));
    assert(!SharedImmutablePayload{}.slice(0, 0));
    bool refused = false;
    try { (void)SharedImmutablePayload::publish(
        std::vector<std::uint8_t>(SharedImmutablePayload::max_slab_bytes + 1)); }
    catch (const std::length_error&) { refused = true; }
    assert(refused);

    SharedPayloadInbox inbox(2, mib * 2);
    assert(inbox.try_send(*slice) == Result::admitted);
    assert(inbox.try_send(root) == Result::admitted);
    assert(inbox.queued() == 2 && inbox.charged_bytes() == mib * 2);
    assert(inbox.try_send(*slice) == Result::full);
    auto received = inbox.try_receive();
    assert(received && received->same_backing(root));
    assert(inbox.queued() == 1 && inbox.charged_bytes() == mib);
    assert(inbox.try_close());
    assert(inbox.queued() == 0 && inbox.charged_bytes() == 0);
    assert(inbox.try_send(root) == Result::closed);
    assert(received->read_byte(0) == 42); // Receiver outlives mailbox.
    assert(!inbox.try_receive());
    assert(inbox.try_close()); // Idempotent shutdown.
    auto closed_probe = inbox.try_receive_result();
    assert(closed_probe.status == SharedPayloadInbox::ReceiveStatus::closed);
    assert(!closed_probe.descriptor);

    // Repeated ring wrap must preserve FIFO, bounded credits and
    // distinction between EMPTY and CLOSED without further slot allocation.
    SharedPayloadInbox ring(3, mib * 3);
    for (int round = 0; round < 512; ++round) {
        auto empty_probe = ring.try_receive_result();
        assert(empty_probe.status == SharedPayloadInbox::ReceiveStatus::empty);
        assert(!empty_probe.descriptor);
        assert(ring.try_send(root) == Result::admitted);
        assert(ring.try_send(*slice) == Result::admitted);
        auto first = ring.try_receive_result();
        assert(first.status == SharedPayloadInbox::ReceiveStatus::received);
        assert(first.descriptor && first.descriptor->size() == mib);
        assert(ring.try_send(root) == Result::admitted);
        auto second = ring.try_receive_result();
        assert(second.status == SharedPayloadInbox::ReceiveStatus::received);
        assert(second.descriptor && second.descriptor->size() == 99);
        auto third = ring.try_receive_result();
        assert(third.status == SharedPayloadInbox::ReceiveStatus::received);
        assert(third.descriptor && third.descriptor->size() == mib);
        assert(ring.queued() == 0 && ring.charged_bytes() == 0);
    }
    assert(ring.try_close());
    assert(ring.try_receive_result().status == SharedPayloadInbox::ReceiveStatus::closed);
    assert(ring.try_send(*slice) == Result::closed);

    SharedPayloadInbox limited(10, mib - 1);
    assert(limited.try_send(*slice) == Result::full); // Small slice retains 1 MiB.
    assert(limited.queued() == 0 && limited.charged_bytes() == 0);
    assert(limited.try_send(SharedImmutablePayload{}) == Result::invalid);

    auto empty = SharedImmutablePayload::publish({});
    assert(empty.valid() && empty.size() == 0 && empty.charged_bytes() == 0);
    empty.with_read_only_span([](auto bytes) { assert(bytes.empty()); });
    assert(empty.slice(0, 0) && !empty.read_byte(0));
    SharedPayloadInbox empty_inbox(1, 0);
    assert(empty_inbox.try_send(empty) == Result::admitted);
    assert(empty_inbox.try_receive()->size() == 0);

    std::atomic<int> failures{0};
    std::array<std::thread, 8> readers;
    for (auto& t : readers) t = std::thread([&] {
        for (int i = 0; i < 5000; ++i) {
            auto copy = root; // Descriptor copy; shared backing, no byte copy.
            auto part = copy.slice((i * 7) % (mib-1), 1);
            if (!part || !part->same_backing(root) || part->read_byte(0) != 42)
                failures.fetch_add(1);
        }
    });
    for (auto& t : readers) t.join();
    assert(failures.load() == 0);

    SharedPayloadInbox per_actor(128, mib * 128);
    for (int i = 0; i < 100; ++i)
        assert(per_actor.try_send(root) == Result::admitted);
    root = SharedImmutablePayload{}; // Sending actor drops its handle.
    for (int i = 0; i < 100; ++i) {
        auto item = per_actor.try_receive();
        assert(item && item->read_byte(0) == 42);
    }
    assert(per_actor.charged_bytes() == 0);
    return 0;
}
