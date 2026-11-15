#include "oreslang/runtime/TrustedPointerWindow.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::NativeRegionIdentity;
using oreslang::runtime::TrustedPointerWindow;

template<class F> void denied(F f) {
    bool failed = false;
    try { f(); } catch (const std::logic_error&) { failed = true; }
    if (!failed) throw std::runtime_error("unexpected pointer authority");
}
void require(bool ok) { if (!ok) throw std::runtime_error("pointer test failure"); }

int main() {
    const ActorHeapIdentity actor{10, 20, 30}, other_actor{10, 21, 30}, stale_epoch{10, 20, 29};
    const auto allocation = NativeRegionIdentity::supervisor_minted(7, 11, 13);
    alignas(std::uint64_t) std::array<std::uint32_t, 4> values{{11, 22, 33, 44}};
    TrustedPointerWindow region(
        actor, allocation, values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    require(region.region_identity() == allocation);
    denied([&] { (void)NativeRegionIdentity::supervisor_minted(0, 11, 13); });
    denied([&] { (void)NativeRegionIdentity::supervisor_minted(7, 0, 13); });
    denied([&] { (void)NativeRegionIdentity::supervisor_minted(7, 11, 0); });
    // The public POD identity can be constructed directly by host code;
    // the window must still reject the trivially forged zero sentinel.
    denied([&] {
        (void)TrustedPointerWindow(actor, NativeRegionIdentity{0, 11, 13},
            values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    });
    denied([&] {
        (void)TrustedPointerWindow(actor, NativeRegionIdentity{7, 0, 13},
            values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    });
    denied([&] {
        (void)TrustedPointerWindow(actor, NativeRegionIdentity{7, 11, 0},
            values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    });
    auto p = region.at<std::uint32_t>(actor);
    require(p.load(actor) == 11);
    require(p.offset(actor, 2).load(actor) == 33);
    p.offset(actor, 3).store(actor, 42);
    require(values[3] == 42);
    require(p.offset(actor, 4).offset(actor, -1).load(actor) == 42);
    denied([&] { (void)p.offset(actor, 4).load(actor); });
    denied([&] { (void)p.offset(actor, 5); });
    denied([&] { (void)p.offset(actor, -1); });
    denied([&] { (void)p.offset(actor, std::numeric_limits<std::ptrdiff_t>::min()); });
    denied([&] { (void)p.offset(actor, std::numeric_limits<std::ptrdiff_t>::max()); });
    denied([&] { (void)p.load(other_actor); });
    denied([&] { (void)p.load(stale_epoch); });
    denied([&] { (void)region.at<std::uint32_t>(other_actor); });
    denied([&] { (void)region.at<std::uint64_t>(actor, 4); });
    require(p.with_ffi_address(actor, [](std::uint32_t* ptr) { return *ptr; }) == 11);

    std::atomic<bool> in_callback{false}, release_callback{false}, revoked{false};
    std::thread borrower([&] {
        p.with_ffi_address(actor, [&](std::uint32_t*) {
            in_callback.store(true, std::memory_order_release);
            while (!release_callback.load(std::memory_order_acquire)) std::this_thread::yield();
        });
    });
    while (!in_callback.load(std::memory_order_acquire)) std::this_thread::yield();
    std::thread closer([&] { region.revoke(); revoked.store(true, std::memory_order_release); });
    // Revocation cannot finish before the callback exits: both hold the mutex.
    require(!revoked.load(std::memory_order_acquire));
    release_callback.store(true, std::memory_order_release);
    borrower.join(); closer.join();
    require(revoked.load(std::memory_order_acquire));
    denied([&] { (void)p.load(actor); });
    denied([&] { p.store(actor, 1); });
    denied([&] { (void)p.offset(actor, 1); });

    TrustedPointerWindow read_only(
        actor, NativeRegionIdentity::supervisor_minted(7, 11, 14),
        values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadOnly);
    auto ro = read_only.at<std::uint32_t>(actor);
    require(ro.load(actor) == 11);
    denied([&] { ro.store(actor, 99); });
    denied([&] { ro.with_ffi_address(actor, [](std::uint32_t* ptr) { *ptr = 99; }); });
    require(values[0] == 11);

    TrustedPointerWindow reentrant(
        actor, NativeRegionIdentity::supervisor_minted(7, 11, 15),
        values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    auto reentrant_cursor = reentrant.at<std::uint32_t>(actor);
    require(reentrant_cursor.with_ffi_address(actor, [&](std::uint32_t*) {
        denied([&] { (void)reentrant_cursor.load(actor); });
        denied([&] {
            (void)reentrant_cursor.with_ffi_address(
                actor, [](std::uint32_t*) { return 0; });
        });
        // Same-thread revoke is deferred until this callback releases its raw address.
        reentrant.revoke();
        return 7;
    }) == 7);
    denied([&] { (void)reentrant_cursor.load(actor); });

    // Exceptions must release the scoped FFI lease; otherwise later access
    // or revocation could remain blocked by stale callback state.
    TrustedPointerWindow throwing(
        actor, NativeRegionIdentity::supervisor_minted(7, 11, 16),
        values.data(), sizeof(values), TrustedPointerWindow::Mode::ReadWrite);
    auto throwing_cursor = throwing.at<std::uint32_t>(actor);
    bool caught_callback_error = false;
    try {
        throwing_cursor.with_ffi_address(actor, [](std::uint32_t*) -> void {
            throw std::runtime_error("ffi callback failed");
        });
    } catch (const std::runtime_error&) {
        caught_callback_error = true;
    }
    require(caught_callback_error);
    require(throwing_cursor.load(actor) == 11);
    throwing.revoke();
    denied([&] { (void)throwing_cursor.load(actor); });

    alignas(std::uint32_t) std::uint32_t device_register = 0;
    TrustedPointerWindow mmio(
        actor, NativeRegionIdentity::supervisor_minted(8, 1, 1),
        &device_register, sizeof(device_register), TrustedPointerWindow::Mode::MmioReadWrite);
    auto reg = mmio.at<std::uint32_t>(actor);
    reg.store(actor, 42);
    require(reg.load(actor) == 42);
    denied([&] { reg.with_ffi_address(actor, [](std::uint32_t*) { return 42; }); });
    denied([&] {
        (void)TrustedPointerWindow(
            actor, NativeRegionIdentity::supervisor_minted(9, 1, 1),
            nullptr, 4, TrustedPointerWindow::Mode::ReadWrite);
    });
    std::cout << "trusted pointer region host-only assertions passed\n";
}
