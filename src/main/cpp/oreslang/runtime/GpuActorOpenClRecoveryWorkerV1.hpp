#pragma once

#include "oreslang/runtime/GpuActorOpenClProviderV2.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace oreslang::gpu_actor {

/**
 * Privileged in-process native recovery worker. Only the native supervisor
 * owns this object; never export request() or a provider pointer to guest FFI.
 *
 * Recoveries that may invoke clFinish run on one dedicated thread. They never
 * run from an actor turn or the nonblocking registry progress_all() path.
 *
 * The bounded queue and provider/turn/dedup key prevent unbounded queued
 * recovery and duplicate concurrent clFinish on the same actor operation.
 * Unknown result does NOT release device buffers or commit staged actor state.
 *
 * Destructor joins the thread: a hung GPU driver can block supervisor process
 * teardown. This design is NOT crash-durable and is NOT a complete process
 * isolation boundary; production requires separately supervised helper process
 * and device-reset recovery. A timeout never means physical quiescence.
 */
class OpenClActorRecoveryWorkerV1 final {
public:
    using Port = OpenClActorProviderPortV2;
    using Registry = GpuActorQuarantineRegistryV1;
    using Turn = ProviderPortV1::Turn;

    enum class Kind {
        UnknownBegin, UnknownKernel, DeviceLoss, ReleaseFailed
    };
    struct Stats {
        std::uint64_t accepted;
        std::uint64_t proven;
        std::uint64_t deferred;
        std::uint64_t rejected;
        std::size_t queued;
        bool active;
    };

    explicit OpenClActorRecoveryWorkerV1(std::size_t capacity)
        : capacity_(capacity) {
        if(capacity == 0 || capacity > 4096)
            throw std::invalid_argument("GPU_RECOVERY_INVALID_CAPACITY");
        thread_ = std::thread([this]{run();});
    }
    OpenClActorRecoveryWorkerV1(const OpenClActorRecoveryWorkerV1&) = delete;
    OpenClActorRecoveryWorkerV1& operator=(const OpenClActorRecoveryWorkerV1&) = delete;

    ~OpenClActorRecoveryWorkerV1() {
        // No force-cancellation of native GPU commands; drop outstanding
        // request tickets without dropping root-owned provider resources.
        {
            std::lock_guard<std::mutex> guard(mu_);
            stopping_=true;
        }
        cv_.notify_all();
        if(thread_.joinable()) thread_.join();
    }

    bool request(std::shared_ptr<Port> port,
                 std::shared_ptr<Registry> registry,
                 Turn turn, Kind kind) {
        if (!port || !registry || !port->matches_recovery_turn(turn))
            throw std::invalid_argument("GPU_RECOVERY_FOREIGN_OR_STALE_TURN");
        const Key key{port.get(),turn.actor_generation,turn.sequence,kind};
        std::lock_guard<std::mutex> guard(mu_);
        if(stopping_) throw std::logic_error("GPU_RECOVERY_WORKER_STOPPED");
        if(queued_keys_.count(key)) return false;
        if(queued_keys_.size() >= capacity_) {
            ++rejected_;
            throw std::logic_error("GPU_RECOVERY_QUEUE_CAPACITY_EXHAUSTED");
        }
        jobs_.push_back(Job{std::move(port),std::move(registry),turn,kind,key});
        try {queued_keys_.emplace(key,true);}
        catch (...) {jobs_.pop_back();throw;}
        ++accepted_;
        cv_.notify_one();
        return true;
    }

    Stats stats() const {
        std::lock_guard<std::mutex> guard(mu_);
        return {accepted_,proven_,deferred_,rejected_,jobs_.size(),active_};
    }
    // A testing/host synchronization primitive, NOT device-completion proof.
    // Production must not infer clFinish success from this method.
    void await_idle() {
        std::unique_lock<std::mutex> guard(mu_);
        idle_cv_.wait(guard,[this]{return !active_ && jobs_.empty();});
    }

private:
    using Key = std::tuple<const Port*,std::uint64_t,std::uint64_t,Kind>;
    struct Job {
        std::shared_ptr<Port> port;
        std::shared_ptr<Registry> registry; // keep supervisor quarantine alive
        Turn turn;
        Kind kind;
        Key key;
    };

    void run() noexcept {
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> guard(mu_);
                cv_.wait(guard,[this]{return stopping_ || !jobs_.empty();});
                if(stopping_ && jobs_.empty()) {
                    idle_cv_.notify_all();
                    return;
                }
                job=std::move(jobs_.front());
                jobs_.pop_front();
                active_=true;
            }

            bool proven=false;
            try {
                if(job.port->matches_recovery_turn(job.turn)) {
                    switch(job.kind) {
                        case Kind::UnknownBegin:
                            proven=job.port->recover_unknown_begin_on_worker();
                            break;
                        case Kind::UnknownKernel:
                            proven=job.port->recover_unknown_kernel_on_worker();
                            break;
                        case Kind::DeviceLoss:
                            proven=job.port->recover_device_loss_on_worker();
                            break;
                        case Kind::ReleaseFailed:
                            proven=job.port->recover_release_failure_on_worker();
                            break;
                    }
                }
            } catch (...) {proven=false;}
            // Deliberately do not poll the registry or publish actor state
            // from the worker thread. The actor scheduler handles that
            // later using the native failed completion receipt.
            {
                std::lock_guard<std::mutex> guard(mu_);
                if(proven) ++proven_;
                else ++deferred_;
                queued_keys_.erase(job.key);
                active_=false;
                if(jobs_.empty()) idle_cv_.notify_all();
            }
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mu_;
    std::condition_variable cv_,idle_cv_;
    std::deque<Job> jobs_;
    std::map<Key,bool> queued_keys_;
    std::thread thread_;
    bool stopping_=false;
    bool active_=false;
    std::uint64_t accepted_=0,proven_=0,deferred_=0,rejected_=0;
};

} // namespace oreslang::gpu_actor
