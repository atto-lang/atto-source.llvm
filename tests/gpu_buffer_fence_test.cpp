#include "oreslang/runtime/GpuBufferFence.hpp"
#include <stdexcept>
#include <iostream>
#include <atomic>
#include <thread>
#include <utility>

template<class Exception, class F> void expects(F fn) {
    try { fn(); } catch (const Exception&) { return; }
    throw std::runtime_error("GPU buffer fence expected rejection did not happen");
}
static void check(bool ok) {
    if (!ok) throw std::runtime_error("GPU buffer fence test failed");
}
int main() {
    using namespace oreslang::gpu;
    const Identity id{"tenant-A", "device-0", 7};
    const Identity foreign{"tenant-B", "device-0", 7};
    BufferFence buffer(id, 1024);
    expects<std::logic_error>([&] { (void)buffer.begin_command(foreign); });
    auto first = buffer.begin_command(id);
    check(buffer.status() == Status::InFlight);
    expects<std::logic_error>([&] { (void)buffer.take(); });
    expects<std::logic_error>([&] { buffer.close_after_retirement(); });
    expects<std::logic_error>([&] { buffer.complete({6, first.sequence}, true); });
    buffer.complete(first, true);
    expects<std::logic_error>([&] { buffer.complete(first, true); });
    auto owner = buffer.take();
    expects<std::logic_error>([&] { (void)buffer.status(); });
    buffer.close_after_retirement(); // moved-from wrapper has no handle
    auto next = owner.begin_command(id);
    check(next.sequence != first.sequence);
    owner.cancel_pending(next);
    check(owner.status() == Status::Poisoned);
    expects<std::logic_error>([&] { (void)owner.take(); });
    expects<std::logic_error>([&] { owner.retire_after_backend_release(id); });
    owner.acknowledge_cancelled_device_event(next, id);
    expects<std::logic_error>([&] { owner.acknowledge_cancelled_device_event(next, id); });
    owner.retire_after_backend_release(id);
    owner.close_after_retirement();
    expects<std::logic_error>([&] { (void)owner.begin_command(id); });

    const Identity second{"tenant", "device", 1};
    BufferFence failed(second, 4);
    auto t = failed.begin_command(second);
    failed.complete(t, false);
    check(failed.status() == Status::Poisoned);
    expects<std::logic_error>([&] { (void)failed.begin_command(second); });
    failed.retire_after_backend_release(second);
    failed.close_after_retirement();
    expects<std::invalid_argument>([&] { (void)BufferFence(second, 0); });

    // Concurrent take() versus reading the old owner used to race on state_:
    // active() returned and take() moved the pointer before the second access.
    // Either operation may win, but never crash or publish a stale snapshot.
    for (int iteration = 0; iteration < 300; ++iteration) {
        BufferFence candidate(Identity{"tenant", "race-device", 50}, 64);
        std::atomic<bool> start{false};
        std::thread observer([&] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            try {
                const auto snapshot = candidate.snapshot();
                check(snapshot.bytes == 64 && snapshot.status == Status::Idle);
            } catch (const std::logic_error&) {
                // take() won; the old wrapper is correctly invalidated.
            }
        });
        start.store(true, std::memory_order_release);
        auto transferred = candidate.take();
        observer.join();
        check(transferred.status() == Status::Idle);
        expects<std::logic_error>([&] { (void)candidate.snapshot(); });
    }

    BufferFence current(Identity{"tenant", "replacement", 60}, 16);
    BufferFence incoming(Identity{"tenant", "replacement", 61}, 16);
    expects<std::logic_error>([&] { current = std::move(incoming); });
    check(current.status() == Status::Idle);
    check(incoming.status() == Status::Idle);
    current.retire_after_backend_release(Identity{"tenant", "replacement", 60});
    current = std::move(incoming);
    check(current.status() == Status::Idle);
    expects<std::logic_error>([&] { (void)incoming.snapshot(); });
    std::cout << "native device fence lifecycle tests passed\n";
}
