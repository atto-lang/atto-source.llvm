#pragma once

#include "oreslang/runtime/WalStorageFdOwner.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace oreslang::runtime {

// Native runtime *host-only* bounded disk worker. Blocking pwrite/fdatasync
// never runs on submit's thread. This is NOT an Oreslang guest builtin or an
// actor carrier integration. Its destructor joins threads: the isolate's
// trusted OFF-CARRIER retirement worker alone may own and destroy this object.
class WalStorageDiskQueue final {
public:
    static constexpr std::size_t kMaxOutstanding = 32;
    using Scope = WalStorageFdOwner::Scope;
    using Receipt = WalStorageFdOwner::SyncedReceipt;

    // Owns the FD owner and therefore its exclusive physical resource until
    // both disk and completion workers have quiesced. The host MUST prove that
    // actor lifetime pins already exist and that destruction is off-carrier.
    WalStorageDiskQueue(std::unique_ptr<WalStorageFdOwner> owner,
                        const void* issuer, Scope expected)
        : WalStorageDiskQueue(std::move(owner),issuer,expected,{}) {}

#ifdef ORES_WAL_HOST_FAULT_TEST
    // Compile-only host test seam. Never export this in production/runtime ABI.
    static std::unique_ptr<WalStorageDiskQueue> for_host_fault_test(
            std::unique_ptr<WalStorageFdOwner> owner, const void* issuer,
            Scope expected, std::function<void()> before_physical) {
        return std::unique_ptr<WalStorageDiskQueue>(new WalStorageDiskQueue(
            std::move(owner),issuer,expected,std::move(before_physical)));
    }
#endif

    WalStorageDiskQueue(const WalStorageDiskQueue&) = delete;
    WalStorageDiskQueue& operator=(const WalStorageDiskQueue&) = delete;
    WalStorageDiskQueue(WalStorageDiskQueue&&) = delete;
    WalStorageDiskQueue& operator=(WalStorageDiskQueue&&) = delete;

    // Never call from a native actor carrier: the caller must be the isolate's
    // off-carrier retirement service. Existing requests may have side effects.
    ~WalStorageDiskQueue() noexcept { retire_on_runtime_worker(); }

    std::future<Receipt> submit(const void* actor_activation,
                                Scope claimed,
                                const std::vector<std::uint8_t>& untrusted_frame) {
        std::promise<Receipt> promise;
        auto future = promise.get_future();
        std::lock_guard<std::mutex> guard(lock_);
        if (stopping_ || revoked_ || poisoned_)
            throw std::logic_error("WAL worker retired/quarantined");
        if (actor_activation != issuer_ || !(claimed == scope_))
            throw std::invalid_argument("foreign native WAL actor activation");
        // Enforce an absolute per-request byte budget BEFORE cloning input;
        // outstanding<=32 then also bounds queued bytes to 35,776.
        if (untrusted_frame.size() < 31 || untrusted_frame.size() > 1118)
            throw std::length_error("WAL frame allocation budget exceeded");
        if (outstanding_ >= kMaxOutstanding)
            throw std::length_error("WAL worker admission limit reached");
        // Copy before queuing. Physical owner independently validates checksum,
        // tenant/generation, op ID, sequence, and 1MiB total WAL budget.
        requests_.push_back(Request{std::vector<std::uint8_t>(untrusted_frame),
                                    std::move(promise)});
        ++outstanding_;
        requestsCv_.notify_one();
        return future;
    }

    // Synchronizes revoke against new admission but cannot undo dispatched I/O.
    void revoke() noexcept {
        {
            std::lock_guard<std::mutex> guard(lock_);
            revoked_ = true;
            owner_->revoke();
        }
        requestsCv_.notify_all();
    }

    bool quarantined() const noexcept {
        std::lock_guard<std::mutex> guard(lock_);
        return poisoned_ || owner_->quarantined();
    }

    std::size_t outstanding() const noexcept {
        std::lock_guard<std::mutex> guard(lock_);
        return outstanding_;
    }

private:
    WalStorageDiskQueue(std::unique_ptr<WalStorageFdOwner> owner,
                        const void* issuer, Scope expected,
                        std::function<void()> test_before_physical)
        : owner_(std::move(owner)), issuer_(issuer), scope_(expected),
          testBeforePhysical_(std::move(test_before_physical)) {
        if (!owner_ || !issuer_) throw std::invalid_argument("missing WAL host grant");
        start_workers();
    }

    struct Request {
        std::vector<std::uint8_t> frame;
        std::promise<Receipt> promise;
    };
    struct Completion {
        std::promise<Receipt> promise;
        std::optional<Receipt> receipt;
        std::exception_ptr failure;
    };

    void start_workers() {
        try {
            diskWorker_ = std::thread([this] { disk_loop(); });
            handoffWorker_ = std::thread([this] { handoff_loop(); });
        } catch (...) {
            revoke();
            {
                std::lock_guard<std::mutex> guard(lock_);
                stopping_ = true;
                handoffStopping_ = true;
            }
            requestsCv_.notify_all();
            handoffCv_.notify_all();
            if (diskWorker_.joinable()) diskWorker_.join();
            if (handoffWorker_.joinable()) handoffWorker_.join();
            throw;
        }
    }

    void disk_loop() noexcept {
        for (;;) {
            std::optional<Request> next;
            {
                std::unique_lock<std::mutex> guard(lock_);
                requestsCv_.wait(guard, [this] { return stopping_ || !requests_.empty(); });
                if (requests_.empty() && stopping_) return;
                next.emplace(std::move(requests_.front()));
                requests_.pop_front();
            }
            Completion completed{std::move(next->promise),std::nullopt,{}};
            try {
                bool allowed;
                {
                    std::lock_guard<std::mutex> guard(lock_);
                    allowed = !revoked_ && !poisoned_;
                }
                if (!allowed)
                    throw WalStorageFdOwner::UnknownOutcome("queued work revoked/quarantined");
                if (testBeforePhysical_) testBeforePhysical_();
                completed.receipt.emplace(owner_->write_and_sync_on_disk_worker(
                    issuer_,scope_,next->frame));
            } catch (...) {
                // Once work was accepted and queued, classify any physical
                // dispatch failure as UNKNOWN; an apparent validation error
                // cannot prove no bytes reached the kernel.
                completed.failure = std::make_exception_ptr(
                    WalStorageFdOwner::UnknownOutcome("queued disk outcome unknown"));
                // Strict quarantine after any queued failure. Even a validation
                // error cannot silently advance or skip this stream's ledger.
                std::lock_guard<std::mutex> guard(lock_);
                poisoned_ = true;
            }
            {
                std::lock_guard<std::mutex> guard(lock_);
                completions_.push_back(std::move(completed));
            }
            handoffCv_.notify_one();
        }
    }

    void handoff_loop() noexcept {
        for (;;) {
            std::optional<Completion> result;
            {
                std::unique_lock<std::mutex> guard(lock_);
                handoffCv_.wait(guard, [this] {
                    return handoffStopping_ || !completions_.empty();
                });
                if (handoffStopping_ && completions_.empty()) return;
                result.emplace(std::move(completions_.front()));
                completions_.pop_front();
            }
            // Promise completion has no inline user callback. No guest closure
            // ever runs on either of these native workers.
            try {
                if (result->failure) result->promise.set_exception(result->failure);
                else result->promise.set_value(std::move(*result->receipt));
            } catch (...) {
                // A broken promise may indicate misuse of the host API. Do
                // not kill the multi-tenant host from a worker's noexcept path.
                std::lock_guard<std::mutex> guard(lock_);
                poisoned_ = true;
            }
            {
                std::lock_guard<std::mutex> guard(lock_);
                --outstanding_;
            }
        }
    }

    void retire_on_runtime_worker() noexcept {
        revoke();
        {
            std::lock_guard<std::mutex> guard(lock_);
            stopping_ = true;
        }
        requestsCv_.notify_all();
        if (diskWorker_.joinable()) diskWorker_.join();
        {
            std::lock_guard<std::mutex> guard(lock_);
            handoffStopping_ = true;
        }
        handoffCv_.notify_all();
        if (handoffWorker_.joinable()) handoffWorker_.join();
    }

    std::unique_ptr<WalStorageFdOwner> owner_;
    const void* issuer_;
    Scope scope_;
    std::function<void()> testBeforePhysical_;
    mutable std::mutex lock_;
    std::condition_variable requestsCv_;
    std::condition_variable handoffCv_;
    std::deque<Request> requests_;
    std::deque<Completion> completions_;
    std::thread diskWorker_;
    std::thread handoffWorker_;
    std::size_t outstanding_ = 0;
    bool stopping_ = false;
    bool handoffStopping_ = false;
    bool revoked_ = false;
    bool poisoned_ = false;
};

} // namespace oreslang::runtime
