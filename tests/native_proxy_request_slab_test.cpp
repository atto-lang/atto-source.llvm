#include "oreslang/runtime/ProxyRequestSlab.hpp"

#include <cstdint>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

using oreslang::runtime::ProxyRequestSlab;

static void require(bool condition) {
    if (!condition) throw std::runtime_error("native proxy slab invariant failed");
}

static void bounded_contract(ProxyRequestSlab::Layout layout, std::uint32_t lanes) {
    std::vector<std::uint32_t> memory(ProxyRequestSlab::required_words(layout, 3, lanes));
    ProxyRequestSlab slab(layout, 3, lanes, memory);
    // JVM and native guests must share signed 32-bit metadata bounds.
    bool epoch_rejected = false;
    try {
        (void)slab.admit(UINT32_C(0x80000000), 10, 100);
    } catch (const std::invalid_argument&) { epoch_rejected = true; }
    require(epoch_rejected && slab.live() == 0);
    bool route_rejected = false;
    try {
        (void)slab.admit(7, 10, UINT32_C(0xffffffff));
    } catch (const std::invalid_argument&) { route_rejected = true; }
    require(route_rejected && slab.live() == 0);
    const auto a = slab.admit(7, 10, 100);
    const auto b = slab.admit(7, 11, 101);
    const auto c = slab.admit(8, 12, 102);
    require(a != 0 && b != 0 && c != 0 && slab.live() == 3);
    require(slab.admit(9, 1, 2) == 0 && slab.full());
    require(slab.route(a) == 100 && slab.connection(b) == 11 && slab.epoch(c) == 8);
    const auto p = slab.pin(a), q = slab.pin(a), z = slab.pin(b);
    require(p && q && z && p != q);
    require(slab.retire(a) && slab.retire(a) && slab.pending(a) == 2);
    require(slab.admit(9, 1, 2) == 0 && slab.settle(q) && !slab.settle(q));
    require(slab.live() == 3 && slab.settle(p) && slab.live() == 2);
    const auto reused = slab.admit(9, 13, 103);
    require(reused != 0 && reused != a && slab.route(reused) == 103);
    require(!slab.retire(a) && !slab.settle(p));
    require(!slab.settle((p & UINT64_C(0xffffffff)) | (UINT64_C(9876) << 32U)));
    require(slab.retire(b) && slab.settle(z) && slab.retire(c) && slab.retire(reused));
    require(slab.live() == 0);
    slab.close();
    for (auto value : memory) require(value == 0);
}

static void serial_exhaustion_contract(ProxyRequestSlab::Layout layout,
                                      std::uint32_t lanes) {
    std::vector<std::uint32_t> memory(
        ProxyRequestSlab::required_words(layout, 1, lanes));
    ProxyRequestSlab slab(layout, 1, lanes, memory);
    const auto ticket = slab.admit(7, 3, 4);
    require(ticket != 0);
    // Inject a legal terminal per-row receipt serial directly into this
    // host-only backing. Do not spin INT32_MAX operations in the CI suite.
    const auto serial_word = layout == ProxyRequestSlab::Layout::SoA
        ? std::size_t{7} : std::size_t{7} * lanes;
    memory[serial_word] = ProxyRequestSlab::kLimit;
    bool rejected = false;
    try { (void)slab.pin(ticket); }
    catch (const std::overflow_error&) { rejected = true; }
    require(rejected && slab.live() == 1);
    require(slab.retire(ticket));
    require(slab.live() == 0 && slab.full());
    // Never recycle a slot that will always fail pin(), even after its
    // last request has retired; bounded backpressure must remain honest.
    require(slab.admit(8, 3, 4) == 0);
    slab.close();
}

static void seeded_churn(ProxyRequestSlab::Layout layout, std::uint32_t lanes,
                         std::uint32_t capacity) {
    std::vector<std::uint32_t> memory(ProxyRequestSlab::required_words(layout, capacity, lanes));
    ProxyRequestSlab slab(layout, capacity, lanes, memory);
    std::vector<std::uint64_t> active(capacity, 0), pins(capacity, 0);
    std::mt19937 random(112358U + capacity);
    for (int n = 0; n < 12000; ++n) {
        const auto i = random() % capacity;
        if (active[i] == 0) {
            active[i] = slab.admit(3, i, i);
            require(active[i] != 0);
        }
        if (pins[i] == 0 && random() % 3 == 0) pins[i] = slab.pin(active[i]);
        if (pins[i] != 0 && random() % 3 == 0) {
            require(slab.settle(pins[i]));
            require(!slab.settle(pins[i]));
            pins[i] = 0;
        }
        if (pins[i] == 0 && random() % 4 == 0) {
            require(slab.retire(active[i]));
            active[i] = 0;
        }
    }
    for (std::uint32_t i = 0; i < capacity; ++i) {
        if (pins[i]) require(slab.settle(pins[i]));
        if (active[i]) require(slab.retire(active[i]));
    }
    require(slab.live() == 0);
    slab.close();
}

int main() {
    try {
        // Static validation must reject unknown discriminants before
        // potentially dividing by a zero/invalid AoSoA lane count.
        bool invalid_layout_rejected = false;
        try {
            (void) ProxyRequestSlab::required_words(
                static_cast<ProxyRequestSlab::Layout>(255), 1, 0);
        } catch (const std::invalid_argument&) {
            invalid_layout_rejected = true;
        }
        require(invalid_layout_rejected);
        bool invalid_tiles_rejected = false;
        try {
            (void) ProxyRequestSlab::required_words(
                ProxyRequestSlab::Layout::AoSoA, 3, 0);
        } catch (const std::invalid_argument&) {
            invalid_tiles_rejected = true;
        }
        require(invalid_tiles_rejected);
        bounded_contract(ProxyRequestSlab::Layout::SoA, 1);
        serial_exhaustion_contract(ProxyRequestSlab::Layout::SoA, 1);
        for (std::uint32_t lanes : {16U, 32U, 64U}) {
            bounded_contract(ProxyRequestSlab::Layout::AoSoA, lanes);
            serial_exhaustion_contract(ProxyRequestSlab::Layout::AoSoA, lanes);
        }
        for (std::uint32_t capacity : {1U, 3U, 16U, 31U, 32U, 33U, 64U, 65U}) {
            seeded_churn(ProxyRequestSlab::Layout::SoA, 1, capacity);
            seeded_churn(ProxyRequestSlab::Layout::AoSoA, 32, capacity);
        }
        std::puts("NATIVE_PROXY_REQUEST_SLAB_OK");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
