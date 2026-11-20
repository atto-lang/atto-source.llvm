// Bounded owned-actor port under contention: only a receiver's explicit
// acknowledgement refunds credits; competing senders never lose ownership
// when admission reports FULL.
#include "oreslang/runtime/BoundedActorPort.hpp"
#include <atomic>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::BoundedActorPort;

int main() {
    constexpr int kProducers = 4;
    constexpr int kMessagesEach = 1500;
    constexpr int kConsumers = 2;
    constexpr int kTotal = kProducers * kMessagesEach;
    BoundedActorPort<std::unique_ptr<int>> port(32, 32 * sizeof(int));
    std::vector<std::atomic<unsigned char>> seen(kTotal);
    for (auto& seen_id : seen) seen_id.store(0, std::memory_order_relaxed);
    std::atomic<int> received{0};
    std::atomic<int> produced{0};
    std::atomic<bool> failed{false};

    std::vector<std::thread> consumers;
    for (int c = 0; c < kConsumers; ++c) {
        consumers.emplace_back([&] {
            while (received.load(std::memory_order_acquire) < kTotal &&
                   !failed.load(std::memory_order_acquire)) {
                auto received_frame = port.try_receive();
                if (!received_frame) {
                    std::this_thread::yield();
                    continue;
                }
                auto& payload = received_frame->value();
                if (!payload || *payload < 0 || *payload >= kTotal) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                const int id = *payload;
                if (seen[static_cast<std::size_t>(id)].fetch_add(
                        1, std::memory_order_acq_rel) != 0) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                // Releasing this checked-out frame, not dequeuing it, must
                // make the next producer admission possible.
                if (!received_frame->acknowledge_released() ||
                    received_frame->acknowledge_released()) {
                    failed.store(true, std::memory_order_release);
                    break;
                }
                received.fetch_add(1, std::memory_order_acq_rel);
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&, p] {
            for (int i = 0; i < kMessagesEach &&
                    !failed.load(std::memory_order_acquire); ++i) {
                const int id = p * kMessagesEach + i;
                auto payload = std::make_unique<int>(id);
                while (!failed.load(std::memory_order_acquire)) {
                    try {
                        if (port.offer(std::move(payload), sizeof(int))) {
                            produced.fetch_add(1, std::memory_order_acq_rel);
                            break;
                        }
                        if (!payload || *payload != id) {
                            failed.store(true, std::memory_order_release);
                            break;
                        }
                        std::this_thread::yield();
                    } catch (...) {
                        failed.store(true, std::memory_order_release);
                    }
                }
            }
        });
    }
    for (auto& producer : producers) producer.join();
    for (auto& consumer : consumers) consumer.join();
    if (failed.load() || produced.load() != kTotal ||
        received.load() != kTotal || port.used_frames() != 0 ||
        port.used_bytes() != 0) {
        throw std::runtime_error("actor port lost frames, payload or credits");
    }
    for (const auto& visited : seen)
        if (visited.load(std::memory_order_acquire) != 1)
            throw std::runtime_error("missing or duplicate actor frame identity");
    port.close();
    auto not_admitted = std::make_unique<int>(kTotal);
    bool rejected = false;
    try { (void)port.offer(std::move(not_admitted), sizeof(int)); }
    catch (const std::logic_error&) { rejected = true; }
    if (!rejected || !not_admitted || *not_admitted != kTotal)
        throw std::runtime_error("closed port admitted or consumed sender ownership");
    std::cout << "BOUNDED_ACTOR_PORT_CONCURRENCY_PASS messages=" << kTotal << '\n';
}
