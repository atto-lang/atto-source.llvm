#include "oreslang/runtime/NativeTimerQueue.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>
#include <algorithm>

using oreslang::runtime::NativeTimerHandle;
using oreslang::runtime::NativeTimerQueue;

static void check(bool ok, const char* explanation) {
    if (!ok) throw std::runtime_error(explanation);
}

int main() {
    {
        NativeTimerQueue queue(2);
        const auto later = queue.arm_after(100, 10);
        const auto sooner = queue.arm_after(100, 5);
        check(queue.pending() == 2, "admission was not counted");
        bool full = false;
        try {
            (void) queue.arm_after(100, 0);
        } catch (const std::length_error&) {
            full = true;
        }
        check(full, "full timer queue accepted unbounded admission");
        check(queue.next_deadline_ns() == 105, "earliest deadline lost");
        check(queue.poll(104, 10).empty(), "future timer fired early");
        auto expired = queue.poll(105, 1);
        check(expired.size() == 1 && expired[0].handle == sooner,
              "poll did not expire earliest deadline first");
        check(!queue.cancel(sooner), "expired timer was cancelled again");
        check(queue.outstanding() == 2, "unclaimed delivery escaped capacity");
        check(queue.claim_delivery(sooner), "polled timer not claimable");
        check(!queue.claim_delivery(sooner), "duplicate delivery claim accepted");
        check(queue.cancel(later), "live timer was not cancellable");
        check(!queue.next_deadline_ns().has_value(), "cancelled deadline leaked");
    }
    {
        NativeTimerQueue first, second;
        const auto target = first.arm_after(10, 5);
        const auto unrelated = second.arm_after(10, 5);
        check(target.id == unrelated.id, "fixture did not reuse local ids");
        check(target.domain != unrelated.domain, "owner domain was reused");
        check(!second.cancel(target), "cross-tenant timer handle cancelled");
        check(!second.claim_delivery(target), "cross-tenant delivery token accepted");
        check(first.cancel(target), "correct owner could not cancel handle");
        check(!first.cancel(target), "stale cancellation succeeded twice");
        second.close();
        check(second.poll(100, 1).empty(), "closed timer owner expired work");
        check(!second.cancel(unrelated), "closed timer owner accepted cancellation");
    }
    {
        NativeTimerQueue queue;
        bool overflow = false;
        try {
            (void) queue.arm_after(std::numeric_limits<std::uint64_t>::max(), 1);
        } catch (const std::overflow_error&) {
            overflow = true;
        }
        check(overflow, "timer deadline overflow was accepted");
        check(queue.pending() == 0, "overflow allocated a timer");
        auto instant = queue.arm_after(200, 0);
        check(queue.poll(200, 0).empty(), "zero-budget poll did work");
        auto due = queue.poll(200, 1);
        check(due.size() == 1 && due[0].handle == instant,
              "zero-delay event did not become ready");
        check(queue.pending() == 0, "expired timer remained scheduled");
        check(queue.outstanding() == 1, "polled ticket was not retained");
        check(!queue.claim_delivery(NativeTimerHandle{instant.domain, instant.id + 1}),
              "unpolled fabricated timer token accepted");
        check(queue.discard_delivery(instant), "retired continuation did not release ticket");
        check(queue.outstanding() == 0, "discarded ticket kept capacity");
    }
    {
        NativeTimerQueue queue(1);
        const auto token = queue.arm_after(100, 0);
        check(queue.poll(100, 1).size() == 1, "ready token not polled");
        bool full = false;
        try { (void) queue.arm_after(100, 0); }
        catch (const std::length_error&) { full = true; }
        check(full, "polled but unclaimed token bypassed capacity");
        queue.close();
        check(!queue.claim_delivery(token), "owner shutdown resurrected polled token");
        check(queue.outstanding() == 0, "shutdown leaked delivery tickets");
    }
    {
        NativeTimerQueue queue;
        for (int i = 0; i < 200; ++i) {
            const auto timer = queue.arm_after(100, 0);
            std::atomic<bool> cancelled{false};
            std::thread other([&] { cancelled = queue.cancel(timer); });
            const auto due = queue.poll(100, 1);
            other.join();
            check((cancelled.load() ? 1 : 0) + static_cast<int>(due.size()) == 1,
                  "concurrent cancel and expiry both won or both lost");
            if (!due.empty()) {
                check(queue.claim_delivery(timer), "raced expiry did not claim once");
                check(!queue.claim_delivery(timer), "raced expiry claimed twice");
            }
        }
        check(queue.outstanding() == 0, "race leaked timer reservations");
        queue.close();
        bool rejected = false;
        try {
            (void) queue.arm_after(0, 1);
        } catch (const std::logic_error&) {
            rejected = true;
        }
        check(rejected, "closed timer owner accepted new work");
    }
    // Stable deadline ordering, bounded poll batches, and capacity held until
    // the owning scheduler explicitly claims or discards every ready ticket.
    {
        NativeTimerQueue queue(128);
        std::vector<NativeTimerHandle> handles;
        for (int i = 0; i < 128; ++i) {
            handles.push_back(queue.arm_after(1'000, 50));
        }
        check(queue.poll(1'049, 128).empty(), "equal-deadline timers fired early");
        check(queue.next_deadline_ns() == 1'050, "same-deadline minimum changed");
        for (std::size_t offset = 0; offset < handles.size(); offset += 7) {
            auto batch = queue.poll(1'050, 7);
            check(batch.size() == std::min<std::size_t>(7, handles.size() - offset),
                  "poll budget was ignored");
            for (std::size_t j = 0; j < batch.size(); ++j) {
                check(batch[j].handle == handles[offset + j],
                      "equal-deadline timers lost FIFO insertion order");
                check(batch[j].deadline_ns == 1'050,
                      "expired ticket lost its deadline");
            }
        }
        check(queue.pending() == 0, "due timers remained pending after bounded polls");
        check(queue.outstanding() == 128, "polled tickets evaded capacity");
        bool full = false;
        try { (void)queue.arm_after(2'000, 10); }
        catch (const std::length_error&) { full = true; }
        check(full, "ready queue bypassed maximum admission");
        for (std::size_t i = 0; i < handles.size(); ++i) {
            const bool consumed = (i % 2 == 0)
                    ? queue.claim_delivery(handles[i])
                    : queue.discard_delivery(handles[i]);
            check(consumed, "ready ticket not consumed exactly once");
            check(!queue.claim_delivery(handles[i]), "stale consumed ticket revived");
        }
        check(queue.outstanding() == 0, "ready capacity leaked after claims/discards");
        check(queue.cancel(queue.arm_after(3'000, 1)),
              "freed capacity could not be reused");
    }

    // Competing scheduler claim and drop must have exactly one linearization
    // winner, even when expiry has already moved the token to ready_.
    {
        NativeTimerQueue queue(1);
        for (int i = 0; i < 300; ++i) {
            const auto token = queue.arm_after(100, 0);
            check(queue.poll(100, 1).size() == 1, "ready claim fixture failed");
            std::atomic<bool> claimed{false};
            std::thread contender([&] {
                claimed.store(queue.claim_delivery(token), std::memory_order_relaxed);
            });
            const bool discarded = queue.discard_delivery(token);
            contender.join();
            check(static_cast<int>(claimed.load(std::memory_order_relaxed))
                    + static_cast<int>(discarded) == 1,
                  "both or neither ready-ticket consumers won");
            check(queue.outstanding() == 0, "concurrent ready claim leaked ticket");
        }
    }

    // Races with close may produce an expiry token but may never grant authority
    // to dispatch guest work after the owner has been revoked.
    {
        for (int i = 0; i < 200; ++i) {
            NativeTimerQueue queue(2);
            const auto due = queue.arm_after(10, 0);
            const auto future = queue.arm_after(10, 100);
            std::thread closer([&] { queue.close(); });
            auto expired = queue.poll(10, 2);
            closer.join();
            check(!queue.claim_delivery(due),
                  "closed owner permitted a late callback claim");
            check(!queue.cancel(future),
                  "closed owner permitted a late timer cancel");
            check(queue.outstanding() == 0, "close/poll left outstanding tickets");
            check(queue.poll(200, 2).empty(), "closed owner returned stale tickets");
            // An expiry returned before close is advisory only; closure revoked it.
            for (const auto& ticket : expired) {
                check(!queue.claim_delivery(ticket.handle),
                      "stale polled token was granted dispatch authority");
            }
        }
    }

    // Large monotonic timestamps and unsigned deadline overflow stay correct.
    {
        NativeTimerQueue queue;
        const auto max = std::numeric_limits<std::uint64_t>::max();
        auto edge = queue.arm_after(max - 2, 2);
        check(queue.poll(max - 1, 1).empty(), "near-overflow timer fired early");
        check(queue.poll(max, 1).size() == 1, "near-overflow timer did not expire");
        check(queue.claim_delivery(edge), "near-overflow delivery was lost");
        bool rejected = false;
        try { (void)queue.arm_after(max - 2, 3); }
        catch (const std::overflow_error&) { rejected = true; }
        check(rejected, "overflow deadline was admitted");
        check(queue.outstanding() == 0, "overflow consumed capacity");
    }

    // Callback handles may revoke a timer even after expiry has been polled,
    // provided CONTROL has not yet claimed execution authority.
    {
        NativeTimerQueue q(1);
        const auto pending = q.arm_after(50, 10);
        check(q.revoke_before_dispatch(pending), "pre-expiry revocation lost");
        check(!q.revoke_before_dispatch(pending), "duplicate revocation accepted");
        check(q.outstanding() == 0, "pre-expiry revoked reservation retained");
        const auto ready = q.arm_after(50, 0);
        check(q.poll(50, 1).size() == 1, "ready fixture failed");
        check(q.revoke_before_dispatch(ready), "polled callback not revocable");
        check(!q.claim_delivery(ready), "revoked callback was dispatched");
        check(!q.discard_delivery(ready), "revoked callback ticket retained");
        check(q.outstanding() == 0, "post-expiry revoked reservation retained");
    }
    // A scheduler claim and a callback cancellation cannot both win.
    {
        NativeTimerQueue q(1);
        for (int i = 0; i < 300; ++i) {
            auto handle = q.arm_after(5, 0);
            check(q.poll(5, 1).size() == 1, "poll fixture failed");
            std::atomic<bool> cancelled{false};
            std::thread other([&] {
                cancelled.store(q.revoke_before_dispatch(handle),
                                std::memory_order_relaxed);
            });
            const bool claimed = q.claim_delivery(handle);
            other.join();
            check(static_cast<int>(claimed)
                    + static_cast<int>(cancelled.load(std::memory_order_relaxed)) == 1,
                  "claim and cancellation did not arbitrate exactly once");
            check(q.outstanding() == 0, "claim/revoke leaked ready reservation");
        }
        q.close();
    }
    // Closing the owner and stale/cross-domain handles cannot revive delivery.
    {
        NativeTimerQueue a(1), b(1);
        const auto token = a.arm_after(0, 0);
        check(a.poll(0, 1).size() == 1, "polled close fixture failed");
        check(!b.revoke_before_dispatch(token), "cross-domain revocation accepted");
        a.close();
        check(!a.revoke_before_dispatch(token), "revocation allowed after close");
        check(!a.claim_delivery(token), "claim allowed after close");
        check(a.outstanding() == 0, "close leaked ticket");
    }

}
