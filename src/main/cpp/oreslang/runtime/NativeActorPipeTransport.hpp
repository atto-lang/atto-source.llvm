#pragma once
#include "oreslang/runtime/ActorPipeContract.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/**
 * Host/control transport substrate for a typed supervised actor pipeline.
 *
 * Each edge is a bounded mailbox. All pending writes/reads settle through
 * host-only futures: guest continuation frames must still re-enter on their
 * owning scheduler CONTROL/USER lane, never inline in a producer thread.
 *
 * Does not lower source PipelineExpr or spawn guest actors.
 */
class NativeActorPipeTransport final {
public:
    NativeActorPipeTransport(
            NativeSupervisorRegistry& registry,
            const NativeSupervisorRegistry::SupervisorRef& supervisor,
            std::vector<PipeStage> stages, std::size_t edge_capacity,
            std::size_t worker_capacity)
        : schema_(check_actor_pipeline(stages)) {
        if (edge_capacity == 0)
            throw std::invalid_argument("native pipeline edge capacity must be positive");
        // Allocate all edges before publishing the supervised group.
        for (std::size_t i = 0; i < schema_.size(); ++i)
            edges_.push_back(std::make_unique<Edge>(edge_capacity));
        group_ = registry.create_group(supervisor, worker_capacity);
    }

    NativeActorPipeTransport(const NativeActorPipeTransport&) = delete;
    NativeActorPipeTransport& operator=(const NativeActorPipeTransport&) = delete;

    ~NativeActorPipeTransport() { close(); }

    std::uint64_t group_id() const noexcept { return group_->group_id(); }
    std::uint64_t supervisor_id() const noexcept { return group_->supervisor_id(); }
    std::size_t edge_count() const noexcept { return edges_.size(); }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }

    /** Write acknowledgement means mailbox admission, NOT actor execution. */
    std::shared_future<void> write_async(std::size_t edge, std::string payload) {
        auto& link = at(edge);
        std::lock_guard<std::mutex> lock(link.mutex);
        if (closed_.load(std::memory_order_acquire) || link.closed)
            return failed_void("native actor pipe is closed");
        if (payload.size() > max_payload_bytes)
            return failed_void("native actor pipe payload exceeds byte budget");
        if (link.messages.size() == link.capacity
                && link.readers.empty()
                && link.writers.size() >= link.capacity)
            return failed_void("native actor pipe pending-write budget exceeded");
        auto pending = std::make_shared<PendingWrite>(std::move(payload));
        auto future = pending->promise.get_future().share();
        if (!link.readers.empty()) {
            auto recipient = std::move(link.readers.front());
            link.readers.pop_front();
            recipient->set_value(std::move(pending->payload));
            pending->promise.set_value();
        } else if (link.messages.size() < link.capacity) {
            link.messages.push_back(std::move(pending->payload));
            pending->promise.set_value();
        } else {
            link.writers.push_back(std::move(pending));
        }
        return future;
    }

    std::shared_future<std::string> read_async(std::size_t edge) {
        auto& link = at(edge);
        std::lock_guard<std::mutex> lock(link.mutex);
        auto receiver = std::make_shared<std::promise<std::string>>();
        auto future = receiver->get_future().share();
        if (!link.messages.empty()) {
            auto payload = std::move(link.messages.front());
            link.messages.pop_front();
            receiver->set_value(std::move(payload));
            // close() publishes cancellation before visiting every edge. An
            // overlapping read may still drain already-admitted data, but
            // must never promote a *pending* writer after that publication.
            // close() will settle the unpromoted writer with an exception.
            if (!closed_.load(std::memory_order_acquire)
                    && !link.closed && !link.writers.empty()) {
                auto waiting = std::move(link.writers.front());
                link.writers.pop_front();
                link.messages.push_back(std::move(waiting->payload));
                waiting->promise.set_value();
            }
        } else if (closed_.load(std::memory_order_acquire) || link.closed) {
            receiver->set_exception(std::make_exception_ptr(
                    std::logic_error("native actor pipe is closed")));
        } else if (link.readers.size() >= link.capacity) {
            receiver->set_exception(std::make_exception_ptr(
                    std::logic_error("native actor pipe pending-read budget exceeded")));
        } else {
            link.readers.push_back(std::move(receiver));
        }
        return future;
    }

    void close() noexcept {
        if (closed_.exchange(true, std::memory_order_acq_rel)) return;
        for (auto& edge : edges_) {
            std::lock_guard<std::mutex> lock(edge->mutex);
            edge->closed = true;
            // Buffered admitted payloads remain readable after closure.
            for (auto& waiting : edge->writers) {
                try { waiting->promise.set_exception(std::make_exception_ptr(
                        std::logic_error("native actor pipe is closed"))); }
                catch (...) { }
            }
            edge->writers.clear();
            for (auto& waiting : edge->readers) {
                try { waiting->set_exception(std::make_exception_ptr(
                        std::logic_error("native actor pipe is closed"))); }
                catch (...) { }
            }
            edge->readers.clear();
        }
        group_->close();
    }

private:
    static constexpr std::size_t max_payload_bytes = 1024 * 1024;

    struct PendingWrite {
        explicit PendingWrite(std::string value) : payload(std::move(value)) {}
        std::string payload;
        std::promise<void> promise;
    };
    struct Edge {
        explicit Edge(std::size_t bound) : capacity(bound) {}
        const std::size_t capacity;
        std::mutex mutex;
        bool closed = false;
        std::deque<std::string> messages;
        std::deque<std::shared_ptr<PendingWrite>> writers;
        std::deque<std::shared_ptr<std::promise<std::string>>> readers;
    };

    Edge& at(std::size_t index) {
        if (index >= edges_.size()) throw std::out_of_range("native actor pipe edge");
        return *edges_[index];
    }

    static std::shared_future<void> failed_void(const char* why) {
        std::promise<void> promise;
        promise.set_exception(std::make_exception_ptr(std::logic_error(why)));
        return promise.get_future().share();
    }

    const std::vector<PipeConnection> schema_;
    std::vector<std::unique_ptr<Edge>> edges_;
    std::shared_ptr<NativeActorGroup> group_;
    std::atomic<bool> closed_{false};
};

} // namespace oreslang::runtime
