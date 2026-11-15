#include "oreslang/runtime/BoundedActorPort.hpp"

#include <memory>
#include <cstdint>
#include <atomic>
#include <thread>
#include <array>
#include <deque>
#include <functional>
#include <random>
#include <type_traits>
#include <utility>
#include <vector>
#include <stdexcept>

using oreslang::runtime::BoundedActorPort;
struct MoveOnly {
    std::unique_ptr<int> item;
    explicit MoveOnly(int n) : item(std::make_unique<int>(n)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly& operator=(MoveOnly&&) noexcept = default;
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
};
static void check(bool ok) { if (!ok) throw std::runtime_error("port invariant broken"); }
template<class F> void rejected(F f) {
    try { f(); } catch (const std::exception&) { return; }
    throw std::runtime_error("expected bounded port failure");
}

struct ReentrantPayload {
    std::function<void()> on_destroy;
    explicit ReentrantPayload(std::function<void()> callback)
        : on_destroy(std::move(callback)) {}
    ReentrantPayload(ReentrantPayload&&) noexcept = default;
    ReentrantPayload(const ReentrantPayload&) = delete;
    ReentrantPayload& operator=(const ReentrantPayload&) = delete;
    ~ReentrantPayload() { if (on_destroy) on_destroy(); }
};

struct ReentrantMove {
    std::function<void()> on_move;
    explicit ReentrantMove(std::function<void()> hook) : on_move(std::move(hook)) {}
    ReentrantMove(ReentrantMove&& other) noexcept
        : on_move(std::move(other.on_move)) {
        if (on_move) on_move();
    }
    ReentrantMove(const ReentrantMove&) = delete;
    ReentrantMove& operator=(const ReentrantMove&) = delete;
};

struct ThrowOnMove {
    static inline bool fail_next_move = false;
    int value;
    explicit ThrowOnMove(int number) : value(number) {}
    ThrowOnMove(ThrowOnMove&& other) {
        if (fail_next_move) throw std::runtime_error("injected move failure");
        value = other.value;
    }
    ThrowOnMove(const ThrowOnMove&) = delete;
};

static_assert(!std::is_move_assignable_v<BoundedActorPort<MoveOnly>::Delivery>,
              "overwriting a live delivery must never strand credits");

int main() {
    BoundedActorPort<MoveOnly> port(1, 64);
    MoveOnly first(42), second(77);
    check(port.offer(std::move(first), 32));
    check(first.item == nullptr);
    check(port.used_frames() == 1 && port.used_bytes() == 32);
    check(!port.offer(std::move(second), 32));
    check(second.item != nullptr); // failed admission retained caller ownership
    auto delivery = port.try_receive();
    check(delivery.has_value() && *delivery->value().item == 42);
    check(port.used_frames() == 1);
    check(!port.offer(std::move(second), 32)); // receiving does not refund
    rejected([&] { port.close(); });
    check(delivery->acknowledge_released());
    check(!delivery->acknowledge_released());
    rejected([&] { (void)delivery->value(); });
    check(port.used_frames() == 0 && port.used_bytes() == 0);
    // close seals admission, including after earlier close failure
    rejected([&] { (void)port.offer(std::move(second), 32); });

    BoundedActorPort<MoveOnly> abandoned(1, 16);
    MoveOnly forgotten(1);
    check(abandoned.offer(std::move(forgotten), 16));
    { auto frame = abandoned.try_receive(); check(frame.has_value()); }
    rejected([&] { abandoned.close(); }); // missing post-fence ack retains quota

    BoundedActorPort<MoveOnly> clean(2, 16);
    MoveOnly a(2), b(3);
    check(clean.offer(std::move(a), 8));
    check(clean.offer(std::move(b), 8));
    auto x = clean.try_receive(), y = clean.try_receive();
    check(x && y);
    check(x->id() != y->id());
    check(x->acknowledge_released() && y->acknowledge_released());
    clean.close();

    // A payload cleanup hook must be allowed to re-enter the port without
    // deadlocking and must see its credit still retained until it quiesces.
    BoundedActorPort<ReentrantPayload> reentrant(1, 8);
    bool callback_ran = false, credit_remained = false;
    ReentrantPayload owns_hook([&] {
        callback_ran = true;
        credit_remained = reentrant.used_frames() == 1 && reentrant.used_bytes() == 8;
        ReentrantPayload denied({});
        credit_remained = credit_remained && !reentrant.offer(std::move(denied), 8);
    });
    check(reentrant.offer(std::move(owns_hook), 8));
    auto receipt = reentrant.try_receive();
    check(receipt && receipt->acknowledge_released());
    check(callback_ran && credit_remained);
    check(reentrant.used_frames() == 0 && reentrant.used_bytes() == 0);
    reentrant.close();

    // User-defined move constructors may re-enter their mailbox. Admission
    // must already own the bounded credits, but the port mutex must be free.
    BoundedActorPort<ReentrantMove> move_reentry(1, 8);
    bool saw_reserved_credits = false, reentrant_rejected = false;
    ReentrantMove move_callback([&] {
        saw_reserved_credits =
                move_reentry.used_frames() == 1 && move_reentry.used_bytes() == 8;
        ReentrantMove nested({});
        reentrant_rejected = !move_reentry.offer(std::move(nested), 8);
    });
    check(move_reentry.offer(std::move(move_callback), 8));
    check(saw_reserved_credits && reentrant_rejected);
    auto moved_receipt = move_reentry.try_receive();
    check(moved_receipt && moved_receipt->id() == 1);
    check(moved_receipt->acknowledge_released());
    move_reentry.close();

    // Closing during an already admitted but still preparing move cannot
    // discard its owned receipt or admit post-close work. Close seals new
    // writes and reports outstanding credits until the old write commits.
    BoundedActorPort<ReentrantMove> close_race(1, 8);
    std::atomic<bool> move_started{false}, release_move{false}, write_done{false};
    ReentrantMove slow_move([&] {
        move_started.store(true, std::memory_order_release);
        while (!release_move.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    std::thread preparing([&] {
        write_done.store(close_race.offer(std::move(slow_move), 8),
                         std::memory_order_release);
    });
    while (!move_started.load(std::memory_order_acquire))
        std::this_thread::yield();
    rejected([&] { close_race.close(); });
    ReentrantMove after_seal({});
    rejected([&] { (void)close_race.offer(std::move(after_seal), 8); });
    release_move.store(true, std::memory_order_release);
    preparing.join();
    check(write_done.load(std::memory_order_acquire));
    check(close_race.used_frames() == 1 && close_race.used_bytes() == 8);
    auto sealed_receipt = close_race.try_receive();
    check(sealed_receipt && sealed_receipt->id() == 1);
    check(sealed_receipt->acknowledge_released());
    close_race.close();

    // A move/allocator failure must not admit a phantom frame, consume an
    // identity, or change sender ownership before publication.
    BoundedActorPort<ThrowOnMove> injected(2, 16);
    ThrowOnMove retry(9);
    ThrowOnMove::fail_next_move = true;
    rejected([&] { (void)injected.offer(std::move(retry), 8); });
    ThrowOnMove::fail_next_move = false;
    check(injected.used_frames() == 0 && injected.used_bytes() == 0);
    check(retry.value == 9);
    check(injected.offer(std::move(retry), 8));
    auto after_failure = injected.try_receive();
    check(after_failure && after_failure->id() == 1);
    check(after_failure->acknowledge_released());
    injected.close();

    // Deterministic adversarial state machine: interleave sends, checked-out
    // deliveries, failed admissions and ACKs. No credit returns on receive.
    BoundedActorPort<MoveOnly> stress(8, 80);
    using Delivery = BoundedActorPort<MoveOnly>::Delivery;
    std::vector<std::unique_ptr<Delivery>> in_flight;
    std::deque<std::pair<int, std::size_t>> queued;
    std::mt19937 random(20261022);
    std::size_t expected_frames = 0, expected_bytes = 0;
    std::uint64_t expected_delivery_id = 0;
    for (int step = 0; step < 20000; ++step) {
        const auto op = random() % 4;
        if (op < 2) {
            const auto bytes = std::size_t(1 + random() % 17);
            MoveOnly next(step);
            const bool should_admit = expected_frames < 8 && bytes <= 80 - expected_bytes;
            const bool admitted = stress.offer(std::move(next), bytes);
            check(admitted == should_admit);
            if (admitted) {
                queued.emplace_back(step, bytes);
                ++expected_frames;
                expected_bytes += bytes;
            } else {
                check(next.item && *next.item == step);
            }
        } else if (op == 2) {
            auto delivered = stress.try_receive();
            check(delivered.has_value() == !queued.empty());
            if (delivered) {
                auto [value, bytes] = queued.front();
                queued.pop_front();
                check(delivered->id() == ++expected_delivery_id);
                check(*delivered->value().item == value);
                check(delivered->charged_bytes() == bytes);
                in_flight.emplace_back(std::make_unique<Delivery>(std::move(*delivered)));
            }
        } else if (!in_flight.empty()) {
            auto index = random() % in_flight.size();
            const auto bytes = in_flight[index]->charged_bytes();
            check(in_flight[index]->acknowledge_released());
            check(!in_flight[index]->acknowledge_released());
            in_flight.erase(in_flight.begin() + index);
            --expected_frames;
            expected_bytes -= bytes;
        }
        check(stress.used_frames() == expected_frames);
        check(stress.used_bytes() == expected_bytes);
    }
    while (auto delivery_to_ack = stress.try_receive()) {
        check(!queued.empty());
        auto [value, bytes] = queued.front();
        queued.pop_front();
        check(delivery_to_ack->id() == ++expected_delivery_id);
        check(*delivery_to_ack->value().item == value);
        check(delivery_to_ack->charged_bytes() == bytes);
        check(delivery_to_ack->acknowledge_released());
        --expected_frames;
        expected_bytes -= bytes;
    }
    for (auto& delivery_to_ack : in_flight) {
        expected_bytes -= delivery_to_ack->charged_bytes();
        --expected_frames;
        check(delivery_to_ack->acknowledge_released());
    }
    check(expected_frames == 0 && expected_bytes == 0);
    check(stress.used_frames() == 0 && stress.used_bytes() == 0);
    stress.close();

    // Deliberately hold a cleanup hook open on another carrier. An offer
    // must fail promptly during the physical quiescence window: no early
    // ACK credit refund, no payload destructor running under the port lock.
    BoundedActorPort<ReentrantPayload> concurrent_cleanup(1, 8);
    std::atomic<bool> cleanup_entered{false}, allow_cleanup{false};
    ReentrantPayload slow_cleanup([&] {
        cleanup_entered.store(true, std::memory_order_release);
        while (!allow_cleanup.load(std::memory_order_acquire))
            std::this_thread::yield();
    });
    check(concurrent_cleanup.offer(std::move(slow_cleanup), 8));
    auto concurrent_receipt = concurrent_cleanup.try_receive();
    check(concurrent_receipt.has_value());
    std::thread cleaner([&] { check(concurrent_receipt->acknowledge_released()); });
    while (!cleanup_entered.load(std::memory_order_acquire))
        std::this_thread::yield();
    ReentrantPayload still_blocked({});
    check(!concurrent_cleanup.offer(std::move(still_blocked), 8));
    allow_cleanup.store(true, std::memory_order_release);
    cleaner.join();
    check(concurrent_cleanup.used_frames() == 0);
    ReentrantPayload accepted_after_cleanup({});
    check(concurrent_cleanup.offer(std::move(accepted_after_cleanup), 8));
    auto retired = concurrent_cleanup.try_receive();
    check(retired && retired->acknowledge_released());
    concurrent_cleanup.close();

    // Four real OS producers race one receiver over a tiny bounded port.
    // Each sender's monotonic sequence must remain FIFO and capacity must
    // never be refunded by dequeue rather than physical ACK.
    BoundedActorPort<MoveOnly> multiproducer(16, 128);
    constexpr int senders = 4, per_sender = 1000;
    std::array<std::thread, senders> producers;
    for (int sender = 0; sender < senders; ++sender) {
        producers[sender] = std::thread([&, sender] {
            for (int seq = 0; seq < per_sender; ++seq) {
                MoveOnly payload(sender * 65536 + seq);
                while (!multiproducer.offer(std::move(payload), 8))
                    std::this_thread::yield();
            }
        });
    }
    std::array<int, senders> last_seen{-1, -1, -1, -1};
    int received = 0;
    while (received < senders * per_sender) {
        auto current = multiproducer.try_receive();
        if (!current) {
            std::this_thread::yield();
            continue;
        }
        const int encoded = *current->value().item;
        const int sender = encoded / 65536, seq = encoded % 65536;
        check(sender >= 0 && sender < senders);
        check(seq == last_seen[sender] + 1);
        last_seen[sender] = seq;
        check(current->acknowledge_released());
        ++received;
    }
    for (auto& producer : producers) producer.join();
    for (auto seq : last_seen) check(seq == per_sender - 1);
    check(multiproducer.used_frames() == 0);
    check(multiproducer.used_bytes() == 0);
    multiproducer.close();
}
