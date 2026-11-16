#pragma once

#include "oreslang/types/AutoTraitRules.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::types {

// Host-independent, compiler-minted proof model. This is NOT source AST
// integration and must never be called with guest-forged AutoType metadata.
enum class ThreadCaptureOrigin {
    CopyScalar, MoveOwned, ShareArc, BorrowShared, BorrowMut,
    ActorState, TaskLocal, ThreadLocal, ForeignHandle, Unknown
};
enum class ThreadCaptureMode { Copy, Move, AtomicShare };
struct ThreadCaptureRequest {
    std::string name;
    ThreadCaptureOrigin origin = ThreadCaptureOrigin::Unknown;
    AutoType type{};
};
struct ThreadCaptureProof {
    std::string name;
    ThreadCaptureMode transfer;
    Capabilities capabilities;
};

/** Captures are admitted only against a specific checked source boundary.
 * A caller may use these proofs for codegen only if it also has:
 *   - trusted AST/provenance derivation (never source-supplied trait flags),
 *   - a successful loan/lifetime/CFG ownership pass at the same epoch,
 *   - verified OS-thread launch authority and structured scope,
 *   - codegen exact-node validation and physical lifetime fences.
 *
 * This API deliberately does not claim any of those additional proofs.
 */
class ThreadCaptureAdmission final {
public:
    static ThreadCaptureAdmission admit(
            const void* boundary, std::uint64_t check_epoch,
            const std::vector<ThreadCaptureRequest>& captures) {
        if (!boundary || !check_epoch)
            throw std::invalid_argument("thread_invalid_boundary");
        if (captures.size() > 256)
            throw std::invalid_argument("thread_capture_budget_exceeded");

        std::unordered_set<std::string> names;
        std::vector<ThreadCaptureProof> proven;
        proven.reserve(captures.size());
        for (const auto& capture : captures) {
            if (capture.name.empty() || capture.name.size() > 1024 ||
                    !names.insert(capture.name).second)
                throw std::invalid_argument("thread_invalid_capture_name");

            if (capture.origin == ThreadCaptureOrigin::BorrowShared ||
                    capture.origin == ThreadCaptureOrigin::BorrowMut)
                throw std::invalid_argument("thread_capture_borrow");
            if (capture.origin == ThreadCaptureOrigin::ActorState)
                throw std::invalid_argument("thread_capture_actor_state");
            if (capture.origin == ThreadCaptureOrigin::TaskLocal ||
                    capture.origin == ThreadCaptureOrigin::ThreadLocal)
                throw std::invalid_argument("thread_capture_execution_local");
            if (capture.origin == ThreadCaptureOrigin::ForeignHandle)
                throw std::invalid_argument("thread_capture_foreign_handle");
            if (capture.origin == ThreadCaptureOrigin::Unknown)
                throw std::invalid_argument("thread_capture_unknown_provenance");

            // Avoid permitting a forged scalar Copy interpretation of
            // ownership-bearing aggregate values.
            if (capture.origin == ThreadCaptureOrigin::CopyScalar &&
                    capture.type.kind != AutoTypeKind::Scalar)
                throw std::invalid_argument("thread_copy_requires_scalar");
            if (capture.origin == ThreadCaptureOrigin::ShareArc &&
                    capture.type.kind != AutoTypeKind::Arc)
                throw std::invalid_argument("thread_share_requires_arc");
            if (capture.origin == ThreadCaptureOrigin::MoveOwned &&
                    capture.type.kind == AutoTypeKind::Arc)
                throw std::invalid_argument("thread_arc_requires_share_admission");

            // Bound recursive metadata before calling AutoTraitRules, which
            // currently has no independent recursion/size guard.
            std::vector<std::pair<const AutoType*, unsigned>> pending = {
                {&capture.type, 0}
            };
            std::size_t inspected = 0;
            while (!pending.empty()) {
                const auto item = pending.back();
                pending.pop_back();
                if (item.second > 64 || ++inspected > 4096)
                    throw std::invalid_argument("thread_capture_type_budget");

                // A type description is compiler-minted metadata, not an
                // arbitrary bag of flags. Validate the *entire* nested shape
                // before using the auto-trait rules: Scalar ignores children,
                // Dyn ignores children, and wrapper rules use exactly_one().
                // Extra metadata must not silently upgrade a Send/Sync proof.
                const AutoType& node = *item.first;
                for (const auto marker : node.dyn_bounds)
                    if (marker != AutoTrait::Send &&
                            marker != AutoTrait::Sync &&
                            marker != AutoTrait::Share)
                        throw std::invalid_argument("thread_capture_invalid_type_shape");
                for (const auto marker : node.negative)
                    if (marker != AutoTrait::Send &&
                            marker != AutoTrait::Sync &&
                            marker != AutoTrait::Share)
                        throw std::invalid_argument("thread_capture_invalid_type_shape");

                const auto invalid_shape = [&] {
                    throw std::invalid_argument("thread_capture_invalid_type_shape");
                };
                switch (node.kind) {
                case AutoTypeKind::Scalar:
                    if (!node.parameters.empty() || !node.dyn_bounds.empty() ||
                            node.complete_nominal_layout || node.shareable_domain)
                        invalid_shape();
                    break;
                case AutoTypeKind::Nominal:
                    if (!node.dyn_bounds.empty()) invalid_shape();
                    break;
                case AutoTypeKind::Dyn:
                    if (!node.parameters.empty() || node.complete_nominal_layout)
                        invalid_shape();
                    break;
                case AutoTypeKind::Box: case AutoTypeKind::Rc:
                case AutoTypeKind::RefCell: case AutoTypeKind::Mutex:
                case AutoTypeKind::RwLock:
                    if (node.parameters.size() != 1 ||
                            !node.dyn_bounds.empty() || node.shareable_domain ||
                            node.complete_nominal_layout)
                        invalid_shape();
                    break;
                case AutoTypeKind::Arc: case AutoTypeKind::Weak:
                    if (node.parameters.size() != 1 ||
                            !node.dyn_bounds.empty() || node.complete_nominal_layout)
                        invalid_shape();
                    break;
                default:
                    invalid_shape();
                }
                // Check the *queued* breadth before allocating it. A
                // maliciously wide nominal type could otherwise enqueue
                // millions of children before the inspected-node guard fires.
                if (node.parameters.size() > 4096 - inspected - pending.size())
                    throw std::invalid_argument("thread_capture_type_budget");
                for (const auto& param : node.parameters)
                    pending.push_back({&param, item.second + 1});
            }

            // The existing auto-trait rules may throw for malformed types.
            // This boundary reports deterministic failure, not a crash.
            Capabilities caps;
            try {
                caps = infer_auto_traits(capture.type);
            } catch (const std::exception&) {
                throw std::invalid_argument("thread_capture_invalid_type");
            }

            if (!caps.send)
                throw std::invalid_argument("thread_capture_non_send");
            if (capture.origin == ThreadCaptureOrigin::ShareArc) {
                if (!caps.sync || !caps.share)
                    throw std::invalid_argument("thread_arc_requires_send_sync_share");
                proven.push_back({capture.name, ThreadCaptureMode::AtomicShare, caps});
            } else if (capture.origin == ThreadCaptureOrigin::CopyScalar) {
                proven.push_back({capture.name, ThreadCaptureMode::Copy, caps});
            } else {
                proven.push_back({capture.name, ThreadCaptureMode::Move, caps});
            }
        }
        return ThreadCaptureAdmission(boundary, check_epoch, std::move(proven));
    }

    void require(const void* boundary, std::uint64_t check_epoch) const {
        if (boundary != boundary_)
            throw std::invalid_argument("thread_capture_wrong_boundary");
        if (check_epoch != epoch_)
            throw std::invalid_argument("thread_capture_stale_epoch");
    }
    const std::vector<ThreadCaptureProof>& captures() const noexcept {
        return captures_;
    }

private:
    ThreadCaptureAdmission(const void* boundary, std::uint64_t epoch,
                           std::vector<ThreadCaptureProof> proofs)
      : boundary_(boundary), epoch_(epoch), captures_(std::move(proofs)) {}
    const void* boundary_;
    std::uint64_t epoch_;
    std::vector<ThreadCaptureProof> captures_;
};

} // namespace oreslang::types
