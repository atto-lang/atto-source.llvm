#pragma once
// Host-side reference substrate, NOT a compiled .ores actor, tenant sandbox or durable provider.
// All guest payload bytes are copied into the sidecar's bounded owned queue.
#include <cstdint>
#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oreslang::runtime::persistence {
enum class WriteKind : std::uint8_t { Snapshot = 1, Event = 2 };
enum class Status : std::uint8_t { Committed, Duplicate, StaleEpoch, VersionConflict, OperationConflict };
struct Identity {
    std::string tenant, actor, slot;
    auto key() const { return std::tie(tenant, actor, slot); }
};
struct Write {
    Identity id;
    std::string operation_id;
    std::uint64_t epoch, expected_version;
    std::uint32_t schema_version;
    WriteKind kind;
    std::vector<std::uint8_t> payload; // owned bytes, never an actor arena pointer
};
struct Receipt {
    std::string operation_id;
    Status status;
    std::uint64_t version;
    bool provider_confirmed_durable = false;
    bool applied() const { return status == Status::Committed || status == Status::Duplicate; }
    bool durable() const { return applied() && provider_confirmed_durable; }
};
inline bool valid_id(const std::string& s) {
    return !s.empty() && s.size() <= 128 && std::none_of(s.begin(),s.end(),
        [](unsigned char c){ return c < 32 || c == 127; });
}

/** A provider must atomically combine state, fencing, and dedup at its commit boundary. */
class Store {
public:
    virtual ~Store() = default;
    virtual void grant_epoch(const std::string&, const std::string&, std::uint64_t) = 0;
    virtual std::vector<Receipt> commit(const std::vector<Write>&) = 0;
};

/** In-memory mock provider: never returns a durable receipt. */
class MemoryStore final : public Store {
    using Actor = std::pair<std::string, std::string>;
    using State = std::tuple<std::string, std::string, std::string>;
    using Operation = std::tuple<std::string, std::string, std::string, std::string>;
    struct Seen { Write write; std::uint64_t version; };
    mutable std::mutex mu_;
    std::map<Actor, std::uint64_t> epochs_;
    std::map<State, std::uint64_t> versions_;
    std::map<State, std::vector<std::uint8_t>> snapshots_;
    std::map<Operation, Seen> operations_;
    std::size_t commit_calls_ = 0;
public:
    void grant_epoch(const std::string& tenant, const std::string& actor, std::uint64_t epoch) {
        if (!valid_id(tenant) || !valid_id(actor) || epoch == 0) throw std::invalid_argument("bad epoch grant");
        std::lock_guard<std::mutex> lock(mu_);
        auto& current = epochs_[{tenant, actor}];
        if (epoch < current) throw std::logic_error("stale epoch grant");
        current = epoch;
    }
    std::vector<Receipt> commit(const std::vector<Write>& writes) {
        std::lock_guard<std::mutex> lock(mu_);
        ++commit_calls_;
        std::vector<Receipt> results;
        results.reserve(writes.size());
        for (const auto& w : writes) {
            Operation op{w.id.tenant, w.id.actor, w.id.slot, w.operation_id};
            // Fencing precedes dedupe so an old activation cannot replay a receipt.
            auto epoch = epochs_.find({w.id.tenant, w.id.actor});
            if (epoch == epochs_.end() || epoch->second != w.epoch) {
                results.push_back({w.operation_id, Status::StaleEpoch, 0}); continue;
            }
            auto seen = operations_.find(op);
            if (seen != operations_.end()) {
                const auto& prior = seen->second.write;
                bool same = prior.epoch == w.epoch && prior.expected_version == w.expected_version &&
                    prior.schema_version == w.schema_version && prior.kind == w.kind &&
                    prior.payload == w.payload;
                results.push_back({w.operation_id, same ? Status::Duplicate : Status::OperationConflict,
                    same ? seen->second.version : 0});
                continue;
            }
            State key{w.id.tenant, w.id.actor, w.id.slot};
            auto& version = versions_[key];
            if (version != w.expected_version || version == std::numeric_limits<std::uint64_t>::max()) {
                results.push_back({w.operation_id, Status::VersionConflict, version}); continue;
            }
            ++version;
            if (w.kind == WriteKind::Snapshot) snapshots_[key] = w.payload;
            operations_.emplace(op, Seen{w, version});
            results.push_back({w.operation_id, Status::Committed, version});
        }
        return results;
    }
    std::uint64_t version(const Identity& id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = versions_.find(id.key());
        return it == versions_.end() ? 0 : it->second;
    }
    std::vector<std::uint8_t> snapshot(const Identity& id) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = snapshots_.find(id.key());
        return it == snapshots_.end() ? std::vector<std::uint8_t>{} : it->second;
    }
    std::size_t commit_calls() const { std::lock_guard<std::mutex> lock(mu_); return commit_calls_; }
};

/** Bounded tenant-scoped writer. flush() runs ONLY on a dedicated host I/O worker. */
class BatchSidecar final {
    std::string tenant_;
    Store& store_;
    std::size_t max_entries_, max_queue_bytes_, max_payload_bytes_, batch_entries_, batch_bytes_;
    std::deque<Write> queue_;
    std::size_t queued_bytes_ = 0;
    std::size_t outstanding_entries_ = 0;
    std::mutex mu_;
    std::mutex flush_mu_; // serializes provider writes in dequeue order
public:
    BatchSidecar(std::string tenant, Store& store, std::size_t max_entries,
                 std::size_t max_queue_bytes, std::size_t max_payload_bytes,
                 std::size_t batch_entries, std::size_t batch_bytes)
        : tenant_(std::move(tenant)), store_(store), max_entries_(max_entries),
          max_queue_bytes_(max_queue_bytes), max_payload_bytes_(max_payload_bytes),
          batch_entries_(batch_entries), batch_bytes_(batch_bytes) {
        if (!valid_id(tenant_) || !max_entries || max_queue_bytes <= 128 ||
            !max_payload_bytes || max_payload_bytes > max_queue_bytes - 128 ||
            !batch_entries || batch_bytes < max_payload_bytes)
            throw std::invalid_argument("invalid sidecar budget");
    }
    // Admission != durability. Caller receives a typed storage receipt only after flush.
    void enqueue(const Write& w) {
        if (w.id.tenant != tenant_) throw std::logic_error("WRONG_TENANT");
        if (!valid_id(w.id.actor) || !valid_id(w.id.slot) || !valid_id(w.operation_id) ||
            w.epoch == 0 || w.schema_version == 0 ||
            (w.kind != WriteKind::Snapshot && w.kind != WriteKind::Event)) throw std::invalid_argument("bad write");
        if (w.payload.size() > max_payload_bytes_) throw std::length_error("PAYLOAD_TOO_LARGE");
        const std::size_t charge = w.payload.size() + 128; // empty writes still consume quota
        std::lock_guard<std::mutex> lock(mu_);
        if (outstanding_entries_ >= max_entries_ || charge > max_queue_bytes_ - queued_bytes_)
            throw std::length_error("BACKPRESSURE");
        queue_.push_back(w); // copies owned payload, never keeps guest pointer
        queued_bytes_ += charge; ++outstanding_entries_;
    }
    std::vector<Receipt> flush() {
        std::lock_guard<std::mutex> only_one_flusher(flush_mu_);
        std::vector<Write> batch;
        std::size_t charge_total = 0;
        {
            std::lock_guard<std::mutex> lock(mu_);
            std::size_t bytes = 0;
            while (!queue_.empty() && batch.size() < batch_entries_ &&
                   queue_.front().payload.size() <= batch_bytes_ - bytes) {
                bytes += queue_.front().payload.size();
                charge_total += queue_.front().payload.size() + 128;
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }
        if (batch.empty()) return {};
        std::vector<Receipt> receipts;
        try {
            receipts = store_.commit(batch);
            if(receipts.size()!=batch.size())throw std::runtime_error("provider receipt cardinality mismatch");
            for(std::size_t i=0;i<batch.size();++i)
                if(receipts[i].operation_id!=batch[i].operation_id ||
                   (receipts[i].durable()&&!receipts[i].applied()))
                    throw std::runtime_error("provider receipt identity mismatch");
        }
        catch (...) {
            std::lock_guard<std::mutex> lock(mu_);
            queued_bytes_ -= charge_total; outstanding_entries_ -= batch.size();
            throw; // unknown commit outcome; caller must retry same op-id
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            queued_bytes_ -= charge_total; outstanding_entries_ -= batch.size();
        }
        return receipts;
    }
    std::size_t queued_count() { std::lock_guard<std::mutex> lock(mu_); return queue_.size(); }
};
} // namespace oreslang::runtime::persistence
