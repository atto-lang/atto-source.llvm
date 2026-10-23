#include "oreslang/runtime/NativeGenerationalReplySlots.hpp"
#include <atomic>
#include <cassert>
#include <string>
#include <thread>
#include <vector>

using oreslang::runtime::NativeGenerationalReplySlots;

template<typename F> void denied(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
}

int main() {
    denied([] { NativeGenerationalReplySlots excessive(65'537); });
    denied([] { NativeGenerationalReplySlots empty(0); });
    NativeGenerationalReplySlots registry(2048);
    const auto actor = registry.endpoint(NativeGenerationalReplySlots::Role::Actor);
    const auto child = registry.endpoint(NativeGenerationalReplySlots::Role::Actor);
    const auto supervisor = registry.endpoint(NativeGenerationalReplySlots::Role::SupervisorApplication);
    auto policy = registry.reserve<std::string>(actor, supervisor);
    auto stats = registry.reserve<int>(supervisor, actor);
    assert(policy.ticket.reply(supervisor, "active"));
    assert(stats.ticket.reply(actor, 42));
    assert(policy.future.get() == "active" && stats.future.get() == 42);
    assert(registry.pending_count() == 0);

    // Admission failure retires the ticket before guest work begins.
    auto rejectedAdmission = registry.reserve<int>(actor, child);
    assert(rejectedAdmission.ticket.fail_admission(actor,
        std::make_exception_ptr(std::runtime_error("mailbox full"))));
    denied([&] { rejectedAdmission.future.get(); });
    assert(!rejectedAdmission.ticket.reply(child, 99));
    assert(registry.pending_count() == 0);

    // Distinct generation even if physical slot is reused.
    auto old = registry.reserve<int>(actor, child);
    const auto oldHandle = old.ticket.local_handle();
    assert(old.ticket.cancel(actor));
    auto fresh = registry.reserve<int>(actor, child);
    assert(oldHandle != fresh.ticket.local_handle());
    assert(!old.ticket.reply(child, 2));
    denied([&] { fresh.ticket.reply(supervisor, 3); });
    denied([&] { fresh.ticket.cancel(child); });
    assert(fresh.ticket.reply(child, 4));
    assert(fresh.future.get() == 4);
    denied([&] { registry.reserve<int>(actor, actor); });

    // Bounded capacity and reverse-order response correlation.
    std::vector<NativeGenerationalReplySlots::Call<int>> pending;
    pending.reserve(2048);
    for (int i = 0; i < 2048; ++i) pending.push_back(registry.reserve<int>(actor, child));
    denied([&] { registry.reserve<int>(actor, child); });
    for (int i = 2047; i >= 0; --i) {
        assert(pending[static_cast<std::size_t>(i)].ticket.reply(child, i));
    }
    for (int i = 0; i < 2048; ++i) {
        assert(pending[static_cast<std::size_t>(i)].future.get() == i);
    }
    assert(registry.pending_count() == 0);

    // One and only one terminal winner.
    std::atomic<int> wins{0};
    for (int i = 0; i < 100; ++i) {
        auto call = registry.reserve<int>(actor, child);
        std::thread responder([&] { if (call.ticket.reply(child, 1)) ++wins; });
        std::thread canceller([&] { if (call.ticket.cancel(actor)) ++wins; });
        responder.join();
        canceller.join();
        assert(registry.pending_count() == 0);
    }
    assert(wins.load() == 100);

    auto abandoned = registry.reserve<int>(actor, child);
    registry.retire(child);
    denied([&] { registry.reserve<int>(actor, child); });
    assert(!abandoned.ticket.reply(child, 100));
    denied([&] { abandoned.future.get(); });
    auto reincarnated = registry.endpoint(NativeGenerationalReplySlots::Role::Actor);
    auto newCall = registry.reserve<std::string>(actor, reincarnated);
    denied([&] { newCall.ticket.reply(child, "old incarnation"); });
    assert(newCall.ticket.timeout(actor));
    denied([&] { newCall.future.get(); });
    registry.close();
    assert(registry.pending_count() == 0);
    denied([&] { registry.reserve<int>(actor, reincarnated); });

    // Ticket outlives registry: weak state prevents use-after-free.
    NativeGenerationalReplySlots::EndpointRef leftoverPeer;
    NativeGenerationalReplySlots::Ticket<int> leftover = [&] {
        NativeGenerationalReplySlots temporary(1);
        auto first = temporary.endpoint(NativeGenerationalReplySlots::Role::Actor);
        auto second = temporary.endpoint(NativeGenerationalReplySlots::Role::Actor);
        leftoverPeer = second;
        return temporary.reserve<int>(first, second).ticket;
    }();
    denied([&] { leftover.reply(child, 8); });
    assert(!leftover.reply(leftoverPeer, 9));
}
