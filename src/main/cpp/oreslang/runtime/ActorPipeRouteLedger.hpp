#pragma once
#include "oreslang/runtime/ActorPipeContract.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::runtime {

/** Host-only, scheduler-owned correlation ledger. This object does not write
 * mailboxes, authenticate actor turns, or enable guest self.* lowering. */
class ActorPipeRouteLedger final {
public:
    class Frame final {
        friend class ActorPipeRouteLedger;
        Frame(const ActorPipeRouteLedger* owner, std::uint64_t generation,
              std::uint64_t flow, std::uint64_t route,
              std::vector<std::string> path, std::size_t at, bool returning)
            : owner_(owner), generation_(generation), flow_(flow), route_(route),
              path_(std::move(path)), at_(at), returning_(returning) {
            if (path_.empty() || at_ >= path_.size())
                throw std::invalid_argument("route frame path index invalid");
        }
        const ActorPipeRouteLedger* owner_;
        std::uint64_t generation_;
        std::uint64_t flow_;
        std::uint64_t route_;
        const std::vector<std::string> path_;
        const std::size_t at_;
        const bool returning_;
        // Conservative accounting of the owned frame and its immutable
        // path buffers. Actual allocator bookkeeping is implementation
        // dependent and therefore not claimed as a hard heap-byte cap.
        std::size_t metadata_charge() const noexcept {
            std::size_t bytes = sizeof(Frame)
                + path_.capacity() * sizeof(std::string) + 64;
            for (const auto& name : path_) bytes += name.capacity();
            return bytes;
        }
    public:
        Frame(const Frame&) = delete;
        Frame& operator=(const Frame&) = delete;
        std::uint64_t generation() const noexcept { return generation_; }
        std::uint64_t flow_id() const noexcept { return flow_; }
        std::uint64_t route_id() const noexcept { return route_; }
        const std::string& receiver() const noexcept { return path_[at_]; }
        bool returning() const noexcept { return returning_; }
    };
    using Handle = std::shared_ptr<const Frame>;

    ActorPipeRouteLedger(const std::vector<PipeStage>& stages,
                         const std::vector<PipeConnection>& edges,
                         std::uint64_t generation, std::size_t max_frames,
                         std::size_t max_metadata_bytes = 64 * 1024 * 1024)
        : edges_(check_actor_pipeline_graph(stages, edges)),
          generation_(generation), max_frames_(max_frames),
          max_metadata_bytes_(max_metadata_bytes) {
        if (!generation_ || !max_frames_ || max_frames_ > 65536)
            throw std::invalid_argument("invalid route generation or frame budget");
        if (!max_metadata_bytes_ || max_metadata_bytes_ > 64 * 1024 * 1024)
            throw std::invalid_argument("invalid route frame metadata byte budget");
        std::unordered_map<std::string, std::size_t> inbound;
        std::unordered_map<std::string, PipeRole> roles;
        for (const auto& stage : stages) {
            const auto id = stage.instance_id.empty() ? stage.actor_type : stage.instance_id;
            inbound.emplace(id, 0);
            roles.emplace(id, stage.role);
        }
        for (std::size_t i = 0; i < edges_.size(); ++i) {
            outgoing_[edges_[i].source].push_back(i);
            ++inbound.at(edges_[i].target);
        }
        for (const auto& entry : inbound) {
            const auto role = roles.at(entry.first);
            if (entry.second == 0 && outgoing_.count(entry.first)
                && (role == PipeRole::readable || role == PipeRole::duplex))
                roots_.insert(entry.first);
        }
        if (roots_.empty()) throw std::invalid_argument("route graph lacks ingress root");
    }

    Handle ingress(const std::string& root) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_open();
        if (!roots_.count(root)) throw std::invalid_argument("unauthorized ingress root");
        reserve(1);
        if (next_flow_ == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("flow identifiers exhausted");
        const auto result = allocate(next_flow_, {root}, 0, false);
        ++next_flow_;
        return result;
    }

    Handle forward(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_forward(frame);
        const auto& out = connected(frame->receiver());
        if (out.size() != 1) throw std::invalid_argument("ambiguous actor forward");
        return child(frame, out.front());
    }

    std::vector<Handle> forward_all(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_forward(frame);
        const auto& out = connected(frame->receiver());
        reserve(out.size());
        reserve_routes(out.size());
        std::vector<Handle> prepared;
        prepared.reserve(out.size());
        std::size_t reserved_bytes = 0;
        for (const auto i : out) {
            auto path = frame->path_;
            if (path.size() >= 1024) throw std::length_error("route path too deep");
            path.push_back(edges_[i].target);
            auto child = Handle(new Frame(this, generation_, frame->flow_,
                next_route_ + prepared.size(), std::move(path), frame->path_.size(), false));
            const auto charge = child->metadata_charge();
            // All-or-none: check tentative batch credits before publication.
            // Previously published branches are never silently rolled back.
            if (charge > max_metadata_bytes_ - active_metadata_bytes_ - reserved_bytes)
                throw std::length_error("route frame metadata budget exhausted");
            reserved_bytes += charge;
            prepared.push_back(std::move(child));
        }
        try {
            for (const auto& value : prepared) active_.emplace(value.get(), value);
        } catch (...) {
            for (const auto& value : prepared) active_.erase(value.get());
            throw;
        }
        active_metadata_bytes_ += reserved_bytes;
        next_route_ += prepared.size();
        return prepared;
    }

    Handle round_robin(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_forward(frame);
        const auto& out = connected(frame->receiver());
        auto& cursor = cursors_[frame->receiver()];
        const auto selected = out[cursor];
        cursor = (cursor + 1) % out.size(); // failed attempt consumes turn
        return child(frame, selected);
    }

    Handle forward_to(const Handle& frame, const std::string& target) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_forward(frame);
        for (const auto i : connected(frame->receiver()))
            if (edges_[i].target == target) return child(frame, i);
        throw std::invalid_argument("target is not an outbound graph edge");
    }

    /** One reverse DATA hop, never an untrusted target selector. Consumes the
     * previous frame exactly once, retaining the original branch path. */
    Handle backward(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_active(frame);
        if (frame->at_ == 0) throw std::logic_error("origin has no upstream route");
        reserve_routes(1);
        auto next = Handle(new Frame(this, generation_, frame->flow_, next_route_,
                                     frame->path_, frame->at_ - 1, true));
        replace_active(frame, next);
        ++next_route_;
        return next;
    }

    /** VM-owned one-shot reply directly to the immutable ingress origin.
     * The full branch path remains sealed inside the frame for audit and
     * correlation. Intermediate actors need not execute synthetic backward
     * turns; a returning frame may never be replied or forwarded again. */
    Handle reply(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_forward(frame);
        if (frame->at_ == 0)
            throw std::logic_error("origin cannot reply to itself");
        reserve_routes(1);
        auto next = Handle(new Frame(this, generation_, frame->flow_,
                                     next_route_, frame->path_, 0, true));
        replace_active(frame, next);
        ++next_route_;
        return next;
    }

    void complete(const Handle& frame) {
        std::lock_guard<std::mutex> lock(mutex_);
        require_active(frame);
        retire(frame.get());
    }

    void retire_if_active(const Handle& frame) noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        if (frame && frame->owner_ == this && frame->generation_ == generation_)
            retire(frame.get());
    }

    std::size_t outstanding() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_.size();
    }
    std::size_t metadata_bytes() const noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        return active_metadata_bytes_;
    }

    void advance_generation(std::uint64_t next) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_successor(next);
        if (!active_.empty()) throw std::logic_error("nonquiescent route generation");
        cursors_.clear();
        generation_ = next;
    }

    std::size_t abort_and_advance(std::uint64_t next) {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_successor(next);
        const auto cancelled = active_.size();
        active_.clear();
        active_metadata_bytes_ = 0;
        cursors_.clear();
        generation_ = next;
        return cancelled;
    }

    void close() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        active_.clear();
        active_metadata_bytes_ = 0;
    }

private:
    const std::vector<PipeConnection> edges_;
    std::unordered_map<std::string, std::vector<std::size_t>> outgoing_;
    std::unordered_set<std::string> roots_;
    mutable std::mutex mutex_;
    // The active ledger MUST retain owning references. A raw-pointer set
    // can preserve a dangling address when callers drop their Handle;
    // allocator reuse would then make a different frame appear active.
    // Releasing a frame is a one-shot completion, cancellation, or STOP.
    std::unordered_map<const Frame*, Handle> active_;
    std::unordered_map<std::string, std::size_t> cursors_;
    std::uint64_t generation_;
    const std::size_t max_frames_;
    const std::size_t max_metadata_bytes_;
    std::size_t active_metadata_bytes_{0};
    std::uint64_t next_flow_{1};
    std::uint64_t next_route_{1};
    bool closed_{false};

    void ensure_open() const {
        if (closed_) throw std::logic_error("route ledger closed");
    }
    void ensure_successor(std::uint64_t next) const {
        ensure_open();
        if (next <= generation_) throw std::invalid_argument("nonincreasing generation");
    }
    void reserve(std::size_t amount) const {
        if (amount > max_frames_ - active_.size())
            throw std::length_error("route frame budget exhausted");
    }
    void reserve_routes(std::size_t amount) const {
        if (amount > std::numeric_limits<std::uint64_t>::max() - next_route_)
            throw std::overflow_error("route ids exhausted");
    }
    void reserve_metadata(std::size_t charge) const {
        if (charge > max_metadata_bytes_ - active_metadata_bytes_)
            throw std::length_error("route frame metadata budget exhausted");
    }
    void retire(const Frame* frame) noexcept {
        const auto it = active_.find(frame);
        if (it != active_.end()) {
            active_metadata_bytes_ -= it->second->metadata_charge();
            active_.erase(it);
        }
    }
    // Replacing a DATA frame with a return frame consumes the old frame's
    // credit only after the new frame is successfully installed.
    // Admission failures preserve the old active frame and its authority.
    void replace_active(const Handle& original, const Handle& next) {
        const auto previous = original->metadata_charge();
        const auto replacement = next->metadata_charge();
        const auto remaining = active_metadata_bytes_ - previous;
        if (replacement > max_metadata_bytes_ - remaining)
            throw std::length_error("route frame metadata budget exhausted");
        active_.emplace(next.get(), next);
        active_.erase(original.get());
        active_metadata_bytes_ = remaining + replacement;
    }
    void require_active(const Handle& frame) const {
        ensure_open();
        if (!frame || frame->owner_ != this || frame->generation_ != generation_
            || !active_.count(frame.get()))
            throw std::logic_error("stale or foreign route frame");
    }
    void require_forward(const Handle& frame) const {
        require_active(frame);
        if (frame->returning_) throw std::logic_error("reverse DATA cannot forward");
    }
    const std::vector<std::size_t>& connected(const std::string& source) const {
        const auto it = outgoing_.find(source);
        if (it == outgoing_.end() || it->second.empty())
            throw std::invalid_argument("source has no outbound route");
        return it->second;
    }
    Handle allocate(std::uint64_t flow, std::vector<std::string> path,
                    std::size_t at, bool returning) {
        reserve_routes(1);
        auto frame = Handle(new Frame(this, generation_, flow, next_route_,
                                      std::move(path), at, returning));
        const auto charge = frame->metadata_charge();
        reserve_metadata(charge);
        active_.emplace(frame.get(), frame);
        active_metadata_bytes_ += charge;
        ++next_route_;
        return frame;
    }
    Handle child(const Handle& current, std::size_t index) {
        reserve(1);
        auto path = current->path_;
        if (path.size() >= 1024) throw std::length_error("route path too deep");
        path.push_back(edges_[index].target);
        return allocate(current->flow_, std::move(path), current->path_.size(), false);
    }
};

} // namespace oreslang::runtime
