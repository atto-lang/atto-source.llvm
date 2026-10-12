#include "oreslang/runtime/GpuBufferFence.hpp"
#include <stdexcept>
#include <iostream>
#include <type_traits>
#include <atomic>
#include <exception>
#include <optional>
#include <thread>

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
    static_assert(!std::is_default_constructible<Ticket>::value,
                  "native device tickets must not be guest-constructible");
    // Identical generation+sequence on another buffer must NOT authenticate
    // completion or cancellation of this buffer's in-flight work.
    BufferFence impostor(id, 1024);
    auto impostor_ticket = impostor.begin_command(id);
    check(impostor_ticket.generation() == first.generation());
    check(impostor_ticket.sequence() == first.sequence());
    expects<std::logic_error>([&] { buffer.complete(impostor_ticket, true); });
    expects<std::logic_error>([&] { buffer.cancel_pending(impostor_ticket); });
    impostor.complete(impostor_ticket, true);
    impostor.retire_after_backend_release(id);
    impostor.close_after_retirement();
    buffer.complete(first, true);
    expects<std::logic_error>([&] { buffer.complete(first, true); });
    auto owner = buffer.take();
    expects<std::logic_error>([&] { (void)buffer.status(); });
    buffer.close_after_retirement(); // moved-from wrapper has no handle
    auto next = owner.begin_command(id);
    check(next.sequence() != first.sequence());
    owner.cancel_pending(next);
    expects<std::logic_error>([&] { owner.acknowledge_cancelled_device_event(first, id); });
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
    // Deterministic concurrent start: a command admission and transfer
    // compete on *the same* owner wrapper. Exactly one may succeed, and
    // ThreadSanitizer must observe no state_ or generation data races.
    for (int trial = 0; trial < 128; ++trial) {
        BufferFence contended(id, 16);
        std::optional<BufferFence> transferred;
        std::optional<Ticket> admitted;
        std::atomic<bool> go{false};
        std::exception_ptr admission_error, transfer_error;
        std::thread command([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try { admitted = contended.begin_command(id); }
            catch (const std::logic_error&) { }
            catch (...) { admission_error = std::current_exception(); }
        });
        std::thread transfer([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            try { transferred.emplace(contended.take()); }
            catch (const std::logic_error&) { }
            catch (...) { transfer_error = std::current_exception(); }
        });
        go.store(true, std::memory_order_release);
        command.join();
        transfer.join();
        if (admission_error) std::rethrow_exception(admission_error);
        if (transfer_error) std::rethrow_exception(transfer_error);
        check(admitted.has_value() != transferred.has_value());
        if (admitted) {
            contended.complete(*admitted, true);
            contended.retire_after_backend_release(id);
            contended.close_after_retirement();
        } else {
            transferred->retire_after_backend_release(id);
            transferred->close_after_retirement();
            expects<std::logic_error>([&] { (void)contended.status(); });
        }
    }
    // Move assignment must not orphan an unretired underlying buffer.
    BufferFence existing(id, 4), incoming(id, 4);
    expects<std::logic_error>([&] { existing = std::move(incoming); });
    check(existing.status() == Status::Idle);
    check(incoming.status() == Status::Idle);
    existing.retire_after_backend_release(id);
    existing.close_after_retirement();
    existing = std::move(incoming);
    check(existing.status() == Status::Idle);
    existing.retire_after_backend_release(id);
    existing.close_after_retirement();
    std::cout << "native device fence lifecycle tests passed\n";
}
