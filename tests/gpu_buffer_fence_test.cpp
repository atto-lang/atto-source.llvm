#include "oreslang/runtime/GpuBufferFence.hpp"
#include <stdexcept>
#include <iostream>
#include <type_traits>

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
    std::cout << "native device fence lifecycle tests passed\n";
}
