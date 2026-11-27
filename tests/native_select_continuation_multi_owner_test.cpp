#include "oreslang/runtime/NativeSelectContinuationFence.hpp"
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <vector>

using Fence = oreslang::runtime::NativeSelectContinuationFence<2>;
static void check(bool b) { if (!b) std::abort(); }
struct Owner {
    std::unique_ptr<Fence> fence = std::make_unique<Fence>();
    Fence::Ticket running = fence->register_continuation(1);
    Fence::Ticket pending = fence->register_continuation(1);
    std::mutex lock;
    std::condition_variable cv;
    bool entered = false, release = false, settled = false;
    std::thread worker;
};
static void multi_owner_settlement() {
    for (int round = 0; round < 10; ++round) {
        std::array<Owner,64> owners;
        for (auto& o : owners) {
            o.worker = std::thread([&o] {
                try {
                    auto lease = o.fence->try_claim(o.running);
                    check(lease.valid());
                    std::unique_lock guard(o.lock);
                    o.entered = true;
                    o.cv.notify_all();
                    check(o.cv.wait_for(guard, std::chrono::seconds(10), [&]{ return o.release; }));
                    throw 42;
                } catch (int value) { check(value == 42); }
                std::lock_guard guard(o.lock);
                o.settled = true;
                o.cv.notify_all();
            });
        }
        for (auto& o : owners) {
            std::unique_lock guard(o.lock);
            check(o.cv.wait_for(guard, std::chrono::seconds(10), [&]{ return o.entered; }));
        }
        for (std::size_t i = 0; i < owners.size(); ++i) {
            auto& o = owners[i];
            check(o.fence->executing() == 1 && o.fence->pending_tickets() == 1);
            check(!o.fence->try_claim(owners[(i+1)%owners.size()].pending).valid());
            check(!o.fence->advance(1));
            if (i % 2 == 0) o.fence.reset();
            else {
                check(o.fence->close());
                check(!o.fence->retirement_ready());
                check(o.fence->executing() == 1);
            }
            check(!o.pending.active() && !o.pending.close());
        }
        for (std::size_t i = owners.size(); i-- > 0;) {
            auto& o = owners[i];
            { std::lock_guard guard(o.lock); o.release = true; }
            o.cv.notify_all();
            o.worker.join();
            check(o.settled);
            if (o.fence) check(o.fence->retirement_ready());
            check(!o.running.active() && !o.pending.active());
            Fence successor(2);
            check(!successor.try_claim(o.running).valid());
            check(!successor.try_claim(o.pending).valid());
            auto fresh = successor.register_continuation(2);
            auto lease = successor.try_claim(fresh);
            check(lease.valid() && lease.close());
            check(successor.close() && successor.retirement_ready());
            if (i > 0) {
                auto& peer = owners[i-1];
                std::lock_guard guard(peer.lock);
                check(!peer.settled);
                if (peer.fence) check(peer.fence->executing() == 1 && !peer.fence->retirement_ready());
            }
        }
    }
}
int main() {
    multi_owner_settlement();
    std::cout << "PASS 640 native owner settlements: 320 owner destruction, 320 explicit STOP; reverse order, stale and foreign tickets denied\n";
}
