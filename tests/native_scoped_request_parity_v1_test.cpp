#include "oreslang/runtime/RequestContextLocalV1.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

using namespace oreslang::runtime;

namespace {
RequestIdentityV1 request_id(std::uint64_t number) {
    // Same actor incarnation; two distinct logical HTTP request scopes.
    return {3, 4, number, 1, {7, 11, 9}};
}

void independent_requests_and_utf8_payload_budgets() {
    auto left_cancel = TaskCancellation::root();
    auto right_cancel = TaskCancellation::root();
    auto left = RequestContextLocalV1::open(request_id(1), left_cancel);
    auto right = RequestContextLocalV1::open(request_id(2), right_cancel);
    assert(left && right);
    RequestLocalKeyV1<std::string> text;
    RequestLocalKeyV1<int> counter;
    assert(left->set(text, std::string(1024, 'x')));
    assert(left->used_bytes() == 1024);
    assert(!left->set(text, std::string(1025, 'y')));
    assert(left->get_copy(text) == std::string(1024, 'x'));
    assert(right->set(text, std::string("independent")));
    assert(right->get_copy(text) == std::string("independent"));
    assert(left->set(text, std::string("\xC3\xA9")));
    assert(left->used_bytes() == 2);
    assert(left->set(counter, 5));
    assert(left->used_bytes() == 2 + sizeof(int));
    assert(left->remove(counter));
    assert(!left->remove(counter));
    assert(left->used_bytes() == 2);
    assert(left_cancel->cancel("http-disconnect"));
    assert(!left->active());
    assert(!left->get_copy(text));
    assert(!left->set(text, std::string("late")));
    assert(left->used_bytes() == 0);
    assert(right->active());
    assert(right->get_copy(text) == std::string("independent"));
    assert(right->close());
    assert(right->used_bytes() == 0);
    left_cancel->retire();
    right_cancel->retire();
}

void exact_aggregate_budget_across_independent_slots() {
    auto cancel = TaskCancellation::root();
    auto request = RequestContextLocalV1::open(request_id(12), cancel);
    assert(request);
    RequestLocalKeyV1<std::string> names[8];
    RequestLocalKeyV1<int> overflow;
    for (auto& key : names)
        assert(request->set(key, std::string(1024, 'b')));
    assert(request->used_bytes() == 8192);
    assert(!request->set(overflow, 1));
    assert(!request->set(names[0], std::string(1025, 'c')));
    assert(request->get_copy(names[0]) == std::string(1024, 'b'));
    assert(request->set(names[0], std::string("short")));
    assert(request->used_bytes() == 8192 - 1024 + 5);
    assert(request->set(overflow, 1));
    assert(request->used_bytes() == 8192 - 1024 + 5 + sizeof(int));
    assert(request->remove(overflow));
    assert(request->used_bytes() == 8192 - 1024 + 5);
    assert(request->close());
    assert(request->used_bytes() == 0);
    cancel->retire();
}

void malformed_unicode_and_pointer_records_fail_closed() {
    auto cancel = TaskCancellation::root();
    auto ctx = RequestContextLocalV1::open(request_id(3), cancel);
    assert(ctx);
    RequestLocalKeyV1<std::string> key;
    assert(ctx->set(key, std::string("safe")));
    // Reject overlong, a lone continuation, UTF-16 surrogate, >U+10FFFF,
    // and truncated 4-byte sequences without changing an existing value.
    const std::string invalid[] = {
        std::string("\xC0\x80"),
        std::string("\x80"),
        std::string("\xED\xA0\x80"),
        std::string("\xF4\x90\x80\x80"),
        std::string("\xF0\x9F")
    };
    for (const auto& sample : invalid) {
        assert(!ctx->set(key, sample));
        assert(ctx->get_copy(key) == std::string("safe"));
        assert(ctx->used_bytes() == 4);
    }
    struct PointerRecord { int* pointer; unsigned long tag; };
    static_assert(std::is_trivially_copyable_v<PointerRecord>);
    int variable = 2;
    RequestLocalKeyV1<PointerRecord> raw;
    assert(!ctx->set(raw, PointerRecord{&variable, 5}));
    assert(!ctx->get_copy(raw));
    assert(ctx->set(key, std::string("\xF0\x9F\x98\x80")));
    assert(ctx->used_bytes() == 4);
    assert(ctx->close());
    cancel->retire();
}

void physical_thread_migration_and_cancel_races() {
    RequestLocalKeyV1<int> key;
    for (std::uint64_t i = 1; i <= 80; ++i) {
        auto cancel = TaskCancellation::root();
        auto ctx = RequestContextLocalV1::open(request_id(i + 20), cancel);
        assert(ctx);
        std::thread first([&] { assert(ctx->set(key, 42)); });
        first.join();
        std::thread migrated([&] {
            assert(ctx->get_copy(key) == 42);
            assert(ctx->set(key, 43));
        });
        migrated.join();
        assert(ctx->get_copy(key) == 43);
        std::atomic<bool> go{false};
        std::thread raced([&] {
            while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int next = 0; next < 40; ++next) {
                if (!ctx->set(key, next)) break;
                (void)ctx->get_copy(key);
            }
        });
        go.store(true, std::memory_order_release);
        assert(cancel->cancel("source-closed"));
        raced.join();
        assert(!ctx->active());
        assert(!ctx->get_copy(key));
        assert(!ctx->set(key, 44));
        assert(!ctx->remove(key));
        cancel->retire();
    }
}
} // namespace

int main() {
    independent_requests_and_utf8_payload_budgets();
    exact_aggregate_budget_across_independent_slots();
    malformed_unicode_and_pointer_records_fail_closed();
    physical_thread_migration_and_cancel_races();
    std::puts("SCOPED_REQUEST_LOCAL_PARITY_HOST_ONLY_PASS");
}
