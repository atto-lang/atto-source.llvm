#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/ActorMailboxCompletion.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>

using oreslang::runtime::ActorMailboxCompletion;
using oreslang::runtime::BoundedActorPort;

#define CHECK(pred) do { if (!(pred)) throw std::runtime_error("native terminal race assertion: " #pred); } while (false)

static void concurrent_ack_and_cancel_are_contiguous() {
    using Status = ActorMailboxCompletion::DrainStatus;
    // Out-of-order terminal cleanup and in-flight ACK race on the shared
    // completion ledger. No test should assume which thread wins.
    for (int trial = 0; trial != 3000; ++trial) {
        auto ledger = std::make_shared<ActorMailboxCompletion>(3);
        BoundedActorPort<int> normal(1, sizeof(int), ledger);
        BoundedActorPort<int> urgent(1, sizeof(int), ledger);
        int first = trial, second = -trial;
        CHECK(normal.offer(std::move(first), sizeof(int)));
        CHECK(urgent.offer(std::move(second), sizeof(int)));
        CHECK(ledger->snapshot() == 2);
        auto processing = normal.try_receive();
        auto abandoned = urgent.try_receive();
        CHECK(processing && processing->sequence() == 1);
        CHECK(abandoned && abandoned->sequence() == 2);
        std::atomic<int> ready{0};
        std::thread acknowledged([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            CHECK(processing->acknowledge_released());
            ledger->complete(processing->sequence());
        });
        std::thread cancelled([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            CHECK(abandoned->acknowledge_released());
            ledger->complete(abandoned->sequence(), true);
        });
        acknowledged.join();
        cancelled.join();
        CHECK(ledger->watermark() == 2);
        CHECK(ledger->drain_status(1) == Status::Delivered);
        CHECK(ledger->drain_status(2) == Status::Aborted);
        CHECK(!ledger->completed_through(2));
        CHECK(normal.used_frames() == 0 && urgent.used_frames() == 0);
        CHECK(normal.used_bytes() == 0 && urgent.used_bytes() == 0);
        CHECK(!processing->acknowledge_released());
        CHECK(!abandoned->acknowledge_released());
        normal.close(); urgent.close();
    }
}

static void owned_send_racing_close_has_exactly_one_winner() {
    // The sender retains its capability when the close fence wins. If the
    // send wins, the receiver gets one receipt; close still prevents
    // subsequent admission even while the successful send is outstanding.
    for (int trial = 0; trial != 3000; ++trial) {
        auto ledger = std::make_shared<ActorMailboxCompletion>(2);
        BoundedActorPort<int> port(1, sizeof(int), ledger);
        auto handle = std::make_unique<int>(trial);
        std::atomic<int> ready{0};
        bool accepted = false, denied = false;
        std::thread writer([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { accepted = port.offer_owned(handle, sizeof(int)); }
            catch (const std::logic_error&) { denied = true; }
        });
        std::thread stopper([&] {
            ready.fetch_add(1, std::memory_order_acq_rel);
            while (ready.load(std::memory_order_acquire) != 2)
                std::this_thread::yield();
            try { port.close(); }
            catch (const std::logic_error&) { }
        });
        writer.join(); stopper.join();
        CHECK((accepted && !denied) || (!accepted && denied));
        if (accepted) {
            CHECK(!handle && ledger->snapshot() == 1);
            CHECK(port.used_frames() == 1);
            auto receipt = port.try_receive();
            CHECK(receipt && receipt->sequence() == 1);
            CHECK(receipt->value() == trial);
            CHECK(receipt->acknowledge_released());
            ledger->complete(receipt->sequence());
        } else {
            CHECK(handle && *handle == trial && ledger->snapshot() == 0);
        }
        CHECK(port.used_frames() == 0 && port.used_bytes() == 0);
        CHECK(ledger->watermark() == ledger->snapshot());
        auto another = std::make_unique<int>(42);
        bool closed = false;
        try { (void)port.offer_owned(another, sizeof(int)); }
        catch (const std::logic_error&) { closed = true; }
        CHECK(closed && another && *another == 42);
        port.close();
    }
}

int main() {
    concurrent_ack_and_cancel_are_contiguous();
    owned_send_racing_close_has_exactly_one_winner();
}
