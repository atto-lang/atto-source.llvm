#include "oreslang/runtime/NativeTimerQueue.hpp"

#include <atomic>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <thread>

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
}
