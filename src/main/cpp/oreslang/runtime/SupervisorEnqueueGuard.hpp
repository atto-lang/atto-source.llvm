#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <utility>

namespace oreslang::runtime {

/**
 * Rejects an enqueue adapter that illegally executes CONTROL inline on the
 * submitting thread while an admission mutex is held.
 *
 * Work scheduled on a different physical carrier may start immediately and
 * simply waits for the lane mutex. Same-thread execution AFTER enqueue()
 * returns is legitimate (e.g. a subsequent cooperative event-loop tick).
 *
 * A rejecting adapter cannot secretly run the callback: inline attempts are
 * suppressed and reported as false to the caller, which fails closed.
 */
struct SupervisorEnqueueGuard final {
    template <class Enqueue>
    static bool submit(const Enqueue& enqueue, std::function<void()> work) {
        auto submitting = std::make_shared<std::atomic<bool>>(true);
        auto ran_inline = std::make_shared<std::atomic<bool>>(false);
        const std::thread::id submitting_thread = std::this_thread::get_id();
        bool accepted = false;
        try {
            accepted = enqueue([work = std::move(work), submitting,
                                ran_inline, submitting_thread]() mutable {
                if (submitting->load(std::memory_order_acquire)
                    && std::this_thread::get_id() == submitting_thread) {
                    ran_inline->store(true, std::memory_order_release);
                    return;
                }
                work();
            });
        } catch (...) {
            submitting->store(false, std::memory_order_release);
            throw;
        }
        submitting->store(false, std::memory_order_release);
        return accepted && !ran_inline->load(std::memory_order_acquire);
    }
};

} // namespace oreslang::runtime
