#include "oreslang/runtime/SharedSymbolLiteralImageV1.hpp"

#include <atomic>
#include <set>
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

// Independent edge-contract suite. These tests exercise the host image ABI
// only; no claim of LLVM guest #literal compilation or actor scheduling.
void audit_native_symbol_image_boundaries() {
    const auto ready = Key::process_symbol("ready");
    const auto error = Key::process_symbol("error");

    // Empty images are valid and do not mint out-of-range readable keys.
    SharedSymbolLiteralImageStore empty;
    const auto empty_generation = empty.publish({});
    auto empty_pin = empty.pin_expected(empty_generation);
    check(empty_pin->size() == 0);
    check(empty_pin->payload_bytes() == 0);
    expect_rejected<std::out_of_range>([&] { (void)empty_pin->at(0); });
    expect_rejected<std::out_of_range>([&] { (void)empty_pin->require_ordinal(ready); });

    // Byte quota inclusivity: 4096 different 256-byte ASCII literals
    // require exactly 1 MiB of immutable logical Symbol payload.
    SharedSymbolLiteralImageStore exact_quota;
    std::vector<Key> full;
    full.reserve(SharedSymbolLiteralImageStore::MAX_LITERALS);
    for (std::size_t i = 0; i < SharedSymbolLiteralImageStore::MAX_LITERALS; ++i) {
        auto prefix = std::string("sym") + std::to_string(i) + "_";
        full.push_back(Key::process_symbol(prefix +
                std::string(256 - prefix.size(), 'x')));
    }
    const auto full_gen = exact_quota.publish(full);
    const auto full_pin = exact_quota.pin_expected(full_gen);
    check(full_pin->size() == SharedSymbolLiteralImageStore::MAX_LITERALS);
    check(full_pin->payload_bytes() == SharedSymbolLiteralImageStore::MAX_PAYLOAD_BYTES);
    check(full_pin->require_ordinal(full.back()) ==
          SharedSymbolLiteralImageStore::MAX_LITERALS - 1);
    check(full_pin->at(4095) == full.back());
    expect_rejected<std::out_of_range>([&] { (void)full_pin->at(4096); });

    // A failed staging attempt leaves the same current image readable.
    SharedSymbolLiteralImageStore rollback;
    const auto generation = rollback.publish({ready, error});
    expect_rejected<std::invalid_argument>([&] {
        (void)rollback.publish({ready, ready});
    });
    expect_rejected<std::invalid_argument>([&] {
        (void)rollback.publish({Key::string("ready")});
    });
    check(rollback.pin_expected(generation)->at(0) == ready);
    check(rollback.live_generation_count() == 1);

    // Nominal domains remain disjoint even for identical UUID-form text.
    const std::string uuid = "550e8400-e29b-41d4-a716-446655440000";
    const auto stable = Key::stable_symbol(uuid);
    const auto process = Key::process_symbol(uuid);
    const auto tagged_generation = rollback.publish({stable, process});
    const auto tagged = rollback.pin_expected(tagged_generation);
    check(tagged->size() == 2);
    check(tagged->require_ordinal(stable) == 0);
    check(tagged->require_ordinal(process) == 1);
    check(!(stable == process));
    expect_rejected<std::invalid_argument>([&] {
        (void)tagged->copy_for_untrusted_core(0);
    });
    expect_rejected<std::invalid_argument>([&] {
        (void)tagged->copy_for_untrusted_core(1);
    });

    // Native NFC is not implemented. Reject non-ASCII input rather than
    // inadvertently claiming parity with canonical Java process Symbols.
    expect_rejected<std::invalid_argument>([&] {
        (void)Key::process_symbol("caf\xC3\xA9");
    });
    expect_rejected<std::invalid_argument>([&] {
        (void)Key::process_symbol("a" + std::string(256, 'a'));
    });
    expect_rejected<std::invalid_argument>([&] {
        (void)Key::stable_symbol("550E8400-E29B-41D4-A716-446655440000");
    });

    // Two old and current pinned generations survive shutdown and retire
    // only after their respective independently owned leases disappear.
    SharedSymbolLiteralImageStore retiring;
    retiring.publish({ready});
    auto old = retiring.pin_latest();
    retiring.publish({error});
    auto recent = retiring.pin_latest();
    retiring.close();
    check(retiring.live_generation_count() == 2);
    check(old->at(0) == ready);
    check(recent->at(0) == error);
    old.reset();
    check(retiring.live_generation_count() == 1);
    recent.reset();
    check(retiring.live_generation_count() == 0);

    // Closed-store rejection precedes the content validator. Retired
    // registries must not inspect or stage a huge untrusted host vector.
    SharedSymbolLiteralImageStore already_closed;
    already_closed.close();
    expect_rejected<std::logic_error>([&] {
        (void)already_closed.publish({Key::string("invalid-Symbol-domain")});
    });
    expect_rejected<std::logic_error>([&] {
        (void)already_closed.publish({ready});
    });
    check(already_closed.live_generation_count() == 0);

    // Immutable reader pins survive destruction of the owning registry.
    SharedSymbolLiteralImageStore::Pin outliving_store;
    std::uint64_t outliving_generation = 0;
    {
        SharedSymbolLiteralImageStore transient;
        outliving_generation = transient.publish({ready, error});
        outliving_store = transient.pin_expected(outliving_generation);
        check(outliving_store->generation() == outliving_generation);
    }
    check(outliving_store->size() == 2);
    check(outliving_store->at(0) == ready);
    check(outliving_store->at(1) == error);
    outliving_store.reset();

    // Concurrent VM-local store creation must never recycle an epoch across
    // stores, even when each store publishes its own generation 1.
    constexpr int threads = 8;
    constexpr int publications = 32;
    std::vector<std::uint64_t> epochs(threads * publications, 0);
    std::atomic<bool> successful{true};
    std::vector<std::thread> publishers;
    for (int t = 0; t < threads; ++t) {
        publishers.emplace_back([&, t] {
            try {
                SharedSymbolLiteralImageStore local;
                for (int i = 0; i < publications; ++i) {
                    const auto epoch = local.publish({ready});
                    epochs[t * publications + i] = epoch;
                    if (!(local.pin_expected(epoch)->at(0) == ready))
                        successful.store(false);
                }
            } catch (...) {
                successful.store(false);
            }
        });
    }
    for (auto& publisher : publishers) publisher.join();
    check(successful.load());
    check(std::set<std::uint64_t>(epochs.begin(), epochs.end()).size() ==
          epochs.size());
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
    audit_native_symbol_image_boundaries();
}
