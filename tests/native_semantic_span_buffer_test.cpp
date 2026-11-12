#include "oreslang/runtime/NativeSemanticSpanBuffer.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <thread>
#include <vector>

using oreslang::runtime::NativeCompletedSpan;
using oreslang::runtime::NativeSemanticSpanBuffer;
using oreslang::runtime::NativeSemanticSpanSlot;
using oreslang::runtime::parse_traceparent_v00;

static NativeCompletedSpan fixture(
        std::uint8_t span_second_byte,
        std::uint16_t operation,
        std::uint8_t kind,
        bool sampled = true) {
    NativeCompletedSpan record{};
    const auto source = sampled
        ? "00-0123456789abcdef0123456789abcdef-abcdef0123456789-01"
        : "00-0123456789abcdef0123456789abcdef-abcdef0123456789-00";
    assert(parse_traceparent_v00(source, record.context));
    record.context.span_id[1] = span_second_byte;
    record.operation_id = operation;
    record.kind = kind;
    record.start_unix_ns = 1780000000000000000ULL;
    record.end_unix_ns = 1780000000000001100ULL;
    record.http_status = 200;
    return record;
}

int main() {
    std::array<NativeSemanticSpanSlot, 3> slots{};
    NativeSemanticSpanBuffer recorder(slots.data(), 3);
    auto server = fixture(1, 201, 2);
    auto actor = fixture(2, 501, 1);
    auto client = fixture(3, 200, 3);
    actor.parent_span_id = server.context.span_id;
    client.parent_span_id = actor.context.span_id;
    NativeCompletedSpan ignored{};

    assert(!recorder.record(server, false));
    assert(!recorder.record(fixture(8, 201, 2, false), true));
    assert(recorder.reserved_count() == 0);
    assert(!recorder.read(0, ignored));

    assert(recorder.record(server, true));
    assert(recorder.record(actor, true));
    assert(recorder.record(client, true));
    assert(!recorder.record(server, true));
    assert(recorder.reserved_count() == 3);
    assert(recorder.dropped_count() == 1);
    assert(recorder.read(0, ignored));
    assert(ignored.operation_id == 201);
    assert(recorder.read(1, ignored));
    assert(ignored.parent_span_id == server.context.span_id);
    assert(recorder.read(2, ignored));
    assert(ignored.parent_span_id == actor.context.span_id);
    assert(!recorder.read(3, ignored));

    auto invalid = fixture(4, 201, 2);
    invalid.context.trace_id.fill(0);
    assert(!recorder.record(invalid, true));
    invalid = fixture(5, 201, 2);
    invalid.end_unix_ns = 1;
    assert(!recorder.record(invalid, true));
    invalid = fixture(6, 0, 2);
    assert(!recorder.record(invalid, true));
    invalid = fixture(7, 201, 2);
    invalid.parent_span_id = invalid.context.span_id;
    assert(!recorder.record(invalid, true)); // W3C span cannot parent itself
    invalid = fixture(8, 201, 2);
    invalid.http_status = 99;
    assert(!recorder.record(invalid, true)); // invalid HTTP response code
    assert(recorder.dropped_count() == 1);

    constexpr std::uint32_t CAPACITY = 64;
    std::array<NativeSemanticSpanSlot, CAPACITY> concurrent_slots{};
    NativeSemanticSpanBuffer concurrent(concurrent_slots.data(), CAPACITY);
    std::atomic<bool> go{false};
    std::vector<std::thread> workers;
    for (int lane = 0; lane < 8; ++lane) {
        workers.emplace_back([&concurrent, &go, lane]() {
            while (!go.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < 100; ++i) {
                auto record = fixture(
                    static_cast<std::uint8_t>(lane + 1), 501, 1);
                (void)concurrent.record(record, true);
            }
        });
    }
    go.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();
    assert(concurrent.reserved_count() == CAPACITY);
    assert(concurrent.dropped_count() == 736);
    for (std::uint32_t i = 0; i < CAPACITY; ++i) {
        assert(concurrent.read(i, ignored));
        assert(ignored.operation_id == 501);
    }

    NativeSemanticSpanBuffer empty(nullptr, 0);
    assert(!empty.record(server, true));
    assert(!empty.read(0, ignored));
    // Do not allow an unchecked caller-provided capacity to wrap/escape the
    // fixed 64 Ki slot budget, even when an array pointer exists.
    std::array<NativeSemanticSpanSlot, 1> small_slots{};
    NativeSemanticSpanBuffer invalid_capacity(small_slots.data(), 65537);
    assert(!invalid_capacity.record(server, true));
    assert(invalid_capacity.reserved_count() == 0);
    return 0;
}
