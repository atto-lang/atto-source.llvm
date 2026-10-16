#pragma once
#include "oreslang/runtime/ActorPipeContract.hpp"
#include "oreslang/runtime/NativeSupervisorRegistry.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
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
    /** Stable edge identity paired with independent, admission-only status.
     * Never interpret a completed admission Future as consumer execution. */
    struct RouteEdgeReceipt {
        std::size_t edge_index;
        std::string target;
        std::shared_future<void> admission;
    };

    NativeActorPipeTransport(
            NativeSupervisorRegistry& registry,
            const NativeSupervisorRegistry::SupervisorRef& supervisor,
            std::vector<PipeStage> stages, std::size_t edge_capacity,
            std::size_t worker_capacity,
            std::size_t buffered_byte_capacity = 64 * 1024 * 1024)
        : schema_(check_actor_pipeline(stages)),
          max_buffered_bytes_(buffered_byte_capacity) {
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
            std::size_t edge_capacity, std::size_t worker_capacity,
            std::size_t buffered_byte_capacity = 64 * 1024 * 1024)
        : schema_(check_actor_pipeline_graph(stages, connections)),
          max_buffered_bytes_(buffered_byte_capacity) {
        initialize(registry, supervisor, edge_capacity, worker_capacity);
    }

    NativeActorPipeTransport(const NativeActorPipeTransport&) = delete;
    NativeActorPipeTransport& operator=(const NativeActorPipeTransport&) = delete;

    ~NativeActorPipeTransport() { close(); }

    std::uint64_t group_id() const noexcept { return group_->group_id(); }
    std::uint64_t supervisor_id() const noexcept { return group_->supervisor_id(); }
    std::size_t edge_count() const noexcept { return edges_.size(); }
    bool closed() const noexcept { return closed_.load(std::memory_order_acquire); }
    /** Host observability: queued message and pending-writer bytes, never
     * bytes already handed to an awaiting reader. */
    std::size_t buffered_payload_bytes() const noexcept {
        return buffered_payload_bytes_.load(std::memory_order_acquire);
    }

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
        // Construct promise/future before charging. An OOM during creation
        // must not leak graph credits. Direct reader handoff is not buffered.
        auto pending = std::make_shared<PendingWrite>(std::move(payload));
        auto future = pending->promise.get_future().share();
        const bool buffered = link.readers.empty();
        const auto charged_bytes = pending->payload.size();
        if (buffered && !try_reserve_buffered_bytes(charged_bytes))
            return failed_void("native actor pipe graph payload byte budget exceeded");
        try {
            if (!buffered) {
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
        } catch (...) {
            // Roll back a not-yet-published reservation on failed admission.
            // Once queued, no following operation may throw in normal flow.
            if (buffered) buffered_payload_bytes_.fetch_sub(
                charged_bytes, std::memory_order_acq_rel);
            throw;
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
            buffered_payload_bytes_.fetch_sub(
                payload.size(), std::memory_order_acq_rel);
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
    std::vector<RouteEdgeReceipt> write_all_report_async(
            const std::string& source, const std::string& payload) {
        const auto& destinations = connected_outlets(source);
        std::vector<RouteEdgeReceipt> results;
        results.reserve(destinations.size());
        // Materialize metadata BEFORE any admission: no allocation of a
        // destination label can interrupt an already-started fan-out batch.
        for (const auto edge : destinations)
            results.push_back({edge, schema_[edge].target, {}});
        // A closed/saturated edge returns a failed admission Future, not a
        // missing entry. Every attempted destination remains identifiable.
        for (auto& result : results) {
            // Individual admission may throw (e.g. allocation failure) after
            // earlier destinations have already accepted the payload. Never
            // discard their receipts or hide partial publication from callers.
            try {
                result.admission = write_async(result.edge_index, payload);
            } catch (...) {
                result.admission = failed_exception(std::current_exception());
            }
        }
        return results;
    }

    /** Compatibility host entry point: report futures in approved edge order. */
    std::vector<std::shared_future<void>> write_all_async(
            const std::string& source, const std::string& payload) {
        auto report = write_all_report_async(source, payload);
        std::vector<std::shared_future<void>> results;
        results.reserve(report.size());
        for (auto& entry : report)
            results.push_back(std::move(entry.admission));
        return results;
    }

    /** Prospective self.forward(value): never silently choose the first of
     * several connected consumers. Branching requires an explicit policy. */
    std::shared_future<void> write_one_async(
            const std::string& source, std::string payload) {
        const auto& destinations = connected_outlets(source);
        if (destinations.size() != 1)
            throw std::invalid_argument(
                "native actor pipe forward is ambiguous: select a routing policy");
        return write_async(destinations.front(), std::move(payload));
    }

    /** Strict per-source round robin, including routes whose write later
     * blocks or fails. No skip-on-backpressure and no hidden retry. */
    std::shared_future<void> write_round_robin_async(
            const std::string& source, std::string payload) {
        const auto& destinations = connected_outlets(source);
        std::lock_guard<std::mutex> selection(route_mutex_);
        const auto cursor = next_outlet_[source];
        const auto edge = destinations[cursor % destinations.size()];
        // An attempted turn counts even if write_async fails before it can
        // return a Future (for example, due to allocation failure). Advancing
        // after admission would violate the strict RR contract in that case.
        next_outlet_[source] = (cursor + 1) % destinations.size();
        return write_async(edge, std::move(payload));
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
        if (key.size() > 4096 || !valid_utf8_key(key))
            throw std::invalid_argument(
                "native actor pipe route key must be bounded canonical UTF-8");
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
                    buffered_payload_bytes_.fetch_sub(
                        waiting->payload.size(), std::memory_order_acq_rel);
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
        if (edge_capacity == 0 || edge_capacity > 4096
                || worker_capacity == 0 || worker_capacity > 4096)
            throw std::invalid_argument("native pipeline queue/worker capacity outside 1..4096");
        if (max_buffered_bytes_ == 0 || max_buffered_bytes_ > 64 * 1024 * 1024)
            throw std::invalid_argument("native pipeline byte capacity outside 1..67108864");
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

    /** Graph-wide credit reservation across independently locked edges.
     * No mutex acquisition across edge locks; failed CAS retries are bounded
     * by concurrent progress. The subtraction form avoids size_t overflow. */
    bool try_reserve_buffered_bytes(std::size_t bytes) noexcept {
        auto used = buffered_payload_bytes_.load(std::memory_order_relaxed);
        while (used <= max_buffered_bytes_
                && bytes <= max_buffered_bytes_ - used) {
            if (buffered_payload_bytes_.compare_exchange_weak(
                    used, used + bytes, std::memory_order_acq_rel,
                    std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    /** Reject overlong encodings, surrogate code points, truncated sequences
     * and out-of-range Unicode before hashing. Key byte identity remains
     * caller-defined; normalization (NFC) is an upstream typed-codec duty. */
    static bool valid_utf8_key(const std::string& key) noexcept {
        for (std::size_t i = 0; i < key.size();) {
            const auto lead = static_cast<unsigned char>(key[i]);
            if (lead <= 0x7f) { ++i; continue; }
            std::size_t width = 0;
            if (lead >= 0xc2 && lead <= 0xdf) width = 2;
            else if (lead >= 0xe0 && lead <= 0xef) width = 3;
            else if (lead >= 0xf0 && lead <= 0xf4) width = 4;
            else return false;
            if (width > key.size() - i) return false;
            for (std::size_t j = 1; j < width; ++j) {
                const auto byte = static_cast<unsigned char>(key[i + j]);
                if ((byte & 0xc0) != 0x80) return false;
            }
            const auto second = static_cast<unsigned char>(key[i + 1]);
            if ((lead == 0xe0 && second < 0xa0) ||
                (lead == 0xed && second > 0x9f) ||
                (lead == 0xf0 && second < 0x90) ||
                (lead == 0xf4 && second > 0x8f)) return false;
            i += width;
        }
        return true;
    }

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

    static std::shared_future<void> failed_exception(std::exception_ptr error) {
        std::promise<void> promise;
        promise.set_exception(std::move(error));
        return promise.get_future().share();
    }

    static std::shared_future<void> failed_void(const char* why) {
        std::promise<void> promise;
        promise.set_exception(std::make_exception_ptr(std::logic_error(why)));
        return promise.get_future().share();
    }

    const std::vector<PipeConnection> schema_;
    const std::size_t max_buffered_bytes_;
    std::atomic<std::size_t> buffered_payload_bytes_{0};
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
