#include "oreslang/runtime/SharedSymbolLiteralImageV1.hpp"

#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using oreslang::symbol_image_v1::SharedSymbolLiteralImageStore;
using oreslang::symbol_property_v1::Key;

namespace {
void check(bool ok) {
    if (!ok) throw std::runtime_error("native shared Symbol image contract failed");
}

template <typename Expected, typename F>
void expect_rejected(F&& operation) {
    bool rejected = false;
    try { operation(); } catch (const Expected&) { rejected = true; }
    check(rejected);
}
}

int main() {
    SharedSymbolLiteralImageStore store;
    const auto ready = Key::process_symbol("ready");
    const auto failed = Key::process_symbol("failed");
    const auto stable = Key::stable_symbol("550e8400-e29b-41d4-a716-446655440000");

    const auto g1 = store.publish({ready, stable});
    // A version minted by another VM-local store must not alias generation 1.
    SharedSymbolLiteralImageStore other_store;
    const auto foreign = other_store.publish({failed});
    check(foreign != g1);
    expect_rejected<std::logic_error>([&] { (void)store.pin_expected(foreign); });
    expect_rejected<std::logic_error>([&] { (void)other_store.pin_expected(g1); });
    auto reader1 = store.pin_expected(g1);
    auto reader2 = store.pin_latest();
    check(reader1.get() == reader2.get()); // physical same immutable image
    check(reader1->size() == 2);
    check(reader1->at(0) == ready);
    check(reader1->require_ordinal(Key::process_symbol("ready")) == 0);
    check(!(ready == Key::string("ready")));
    check(!(Key::process_symbol(stable.name()) == stable));
    check(reader1->copy_for_untrusted_core(0) == ready);
    expect_rejected<std::invalid_argument>([&] {
        (void)reader1->copy_for_untrusted_core(1);
    });
    expect_rejected<std::out_of_range>([&] {
        (void)reader1->require_ordinal(failed);
    });

    expect_rejected<std::invalid_argument>([&] {
        (void)store.publish({ready, ready});
    });
    expect_rejected<std::invalid_argument>([&] {
        (void)store.publish({Key::string("ready")});
    });
    expect_rejected<std::invalid_argument>([&] {
        std::vector<Key> too_many;
        too_many.reserve(SharedSymbolLiteralImageStore::MAX_LITERALS + 1);
        for (std::size_t i = 0; i <= SharedSymbolLiteralImageStore::MAX_LITERALS; ++i)
            too_many.push_back(Key::process_symbol("sym" + std::to_string(i)));
        (void)store.publish(too_many);
    });

    const auto g2 = store.publish({failed});
    check(g2 > g1);
    check(reader1->at(0) == ready); // pinned old image remains valid
    expect_rejected<std::logic_error>([&] { (void)store.pin_expected(g1); });
    check(store.pin_expected(g2)->at(0) == failed);
    check(store.live_generation_count() == 2);
    reader1.reset();
    reader2.reset();
    check(store.live_generation_count() == 1);

    // Hold eight generations; a ninth must be rejected atomically.
    std::vector<SharedSymbolLiteralImageStore::Pin> pinned;
    for (std::size_t i = 0; i < SharedSymbolLiteralImageStore::MAX_LIVE_GENERATIONS; ++i) {
        store.publish({Key::process_symbol("gen" + std::to_string(i))});
        pinned.push_back(store.pin_latest());
    }
    expect_rejected<std::logic_error>([&] {
        (void)store.publish({Key::process_symbol("overflow")});
    });
    check(store.live_generation_count() == SharedSymbolLiteralImageStore::MAX_LIVE_GENERATIONS);
    pinned.clear();
    check(store.live_generation_count() == 1);
    check(store.publish({ready}) > g2);

    // A temporary pin may expire immediately: at() must return an owned
    // key instead of a dangling reference into the retired generation.
    Key copied_from_temporary_pin = store.pin_latest()->at(0);
    check(copied_from_temporary_pin == ready);
    // Pins own immutable state independently from store shutdown, while
    // live_generation_count() must continue to account retained bytes.
    auto pinned_after_close = store.pin_latest();
    store.close();
    store.close();
    check(pinned_after_close->at(0) == ready);
    check(store.live_generation_count() == 1);
    expect_rejected<std::logic_error>([&] { (void)store.pin_latest(); });
    expect_rejected<std::logic_error>([&] { (void)store.publish({ready}); });
    pinned_after_close.reset();
    check(store.live_generation_count() == 0);
    check(copied_from_temporary_pin == ready);

    // No global mutable intern table: concurrently read distinct snapshots.
    SharedSymbolLiteralImageStore concurrent;
    concurrent.publish({ready, Key::process_symbol("error")});
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&] {
            for (int j = 0; j < 1000; ++j) {
                auto pin = concurrent.pin_latest();
                if (!(pin->at(0) == ready) || pin->size() != 2)
                    throw std::runtime_error("corrupted shared Symbol image");
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(concurrent.live_generation_count() == 1);
}
