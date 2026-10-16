#pragma once
#include "oreslang/runtime/ActorPipeContract.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
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
        initialize(registry, supervisor, edge_capacity, worker_capacity);
    }

    /**
     * Fan-out DAG constructor. Every edge is nominally typed, unique, directed
     * and cycle-free. This remains a trusted host transport; guest method
     * lowering must additionally enforce ownership and actor-context rules.
     */
    NativeActorPipeTransport(
            NativeSupervisorRegistry& registry,
            const NativeSupervisorRegistry::SupervisorRef& supervisor,
            std::vector<PipeStage> stages,
            std::vector<PipeConnection> connections,
            std::size_t edge_capacity, std::size_t worker_capacity)
        : schema_(check_actor_pipeline_graph(stages, connections)) {
        initialize(registry, supervisor, edge_capacity, worker_capacity);
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
        // Shared admission and exclusive closure share one barrier. Writes
        // on independent edges proceed concurrently, but none can publish
        // DATA after terminal close begins, even on another edge.
        std::shared_lock<std::shared_mutex> publication(publication_mutex_);
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
        // Admission, promise settlement and the close publication share
        // one barrier. A write cannot acknowledge DATA after terminal close
        // has been published, even on a different edge.
        std::shared_lock<std::shared_mutex> publication(publication_mutex_);
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

    /**
     * Transport realization of prospective self.forward_all(value):
     * deliver one independent immutable/copyable payload to each connected
     * consumer. Results are in validated graph-edge order and acknowledge
     * only mailbox admission, NEVER consumer execution.
     *
     * IMPORTANT: This is NOT a transaction. A full/closed edge can fail
     * after other edges accepted a copy. Callers must examine every Future
     * and implement application-level deduplication/compensation as needed.
     */
    std::vector<std::shared_future<void>> write_all_async(
            const std::string& source, const std::string& payload) {
        const auto& destinations = connected_outlets(source);
        std::vector<std::shared_future<void>> results;
        results.reserve(destinations.size());
        for (const auto edge : destinations)
            results.push_back(write_async(edge, payload));
        return results;
    }

    /** Strict per-source round robin, including routes whose write later
     * blocks or fails. No skip-on-backpressure and no hidden retry. */
    std::shared_future<void> write_round_robin_async(
            const std::string& source, std::string payload) {
        const auto& destinations = connected_outlets(source);
        std::lock_guard<std::mutex> selection(route_mutex_);
        const auto cursor = next_outlet_[source];
        const auto edge = destinations[cursor % destinations.size()];
        auto completion = write_async(edge, std::move(payload));
        next_outlet_[source] = (cursor + 1) % destinations.size();
        return completion;
    }

    /** Named destination selection is restricted to an existing source edge;
     * a caller cannot use another actor's outbound port by guessing a name. */
    std::shared_future<void> write_to_async(
            const std::string& source, const std::string& target,
            std::string payload) {
        const auto& destinations = connected_outlets(source);
        for (const auto edge : destinations)
            if (schema_[edge].target == target)
                return write_async(edge, std::move(payload));
        throw std::invalid_argument("native actor pipe target is not connected to source");
    }

    /** Stable FNV-1a routing for key affinity across process restarts.
     * Not cryptographic: never use key hashing as authorization. */
    std::shared_future<void> write_by_key_async(
            const std::string& source, const std::string& key,
            std::string payload) {
        if (key.size() > 4096)
            throw std::invalid_argument("native actor pipe route key too large");
        const auto& destinations = connected_outlets(source);
        std::uint64_t digest = UINT64_C(14695981039346656037);
        for (const unsigned char byte : key) {
            digest ^= byte;
            digest *= UINT64_C(1099511628211);
        }
        return write_async(destinations[digest % destinations.size()],
                           std::move(payload));
    }

    void close() noexcept {
        {
            // Serialize terminal publication with ALL admissions, not just
            // per-edge closure. A concurrent close() cannot return before
            // every pending transport Future has been settled.
            std::unique_lock<std::shared_mutex> publication(publication_mutex_);
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
        }
        // Dispatch host-owned STOP callbacks outside the admission barrier:
        // these can re-enter a pipe endpoint and must never deadlock the gate.
        group_->close();
    }

private:
    void initialize(
            NativeSupervisorRegistry& registry,
            const NativeSupervisorRegistry::SupervisorRef& supervisor,
            std::size_t edge_capacity, std::size_t worker_capacity) {
        if (edge_capacity == 0)
            throw std::invalid_argument("native pipeline edge capacity must be positive");
        // Allocate all independently bounded edges before publishing the
        // supervised group. Outbound order is caller-defined graph order.
        edges_.reserve(schema_.size());
        for (std::size_t i = 0; i < schema_.size(); ++i) {
            edges_.push_back(std::make_unique<Edge>(edge_capacity));
            outgoing_[schema_[i].source].push_back(i);
        }
        group_ = registry.create_group(supervisor, worker_capacity);
    }

    const std::vector<std::size_t>& connected_outlets(
            const std::string& source) const {
        const auto it = outgoing_.find(source);
        if (it == outgoing_.end() || it->second.empty())
            throw std::invalid_argument("native actor pipe source has no downstream consumers");
        return it->second;
    }

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
    std::unordered_map<std::string, std::vector<std::size_t>> outgoing_;
    // Held only for deterministic round-robin selection, never on guest
    // execution or across a blocking wait on a Future.
    std::mutex route_mutex_;
    std::unordered_map<std::string, std::size_t> next_outlet_;
    std::shared_ptr<NativeActorGroup> group_;
    // Readers/writers on separate edges run in parallel; only closure gets
    // the exclusive terminal publication barrier. Never held for host STOP.
    std::shared_mutex publication_mutex_;
    std::atomic<bool> closed_{false};
};

} // namespace oreslang::runtime
