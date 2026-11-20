#pragma once

#include "oreslang/types/AutoTraitRules.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oreslang::types {

enum class TaskExecutionDomain { Cpu, Gpu };
enum class TaskCaptureSource {
    CopyValue,
    OwnedValue,
    BorrowShared,
    BorrowMut,
    ActorSelf,
    Unknown
};
enum class TaskCaptureTransfer { Copy, Move };

struct TaskCaptureRequest {
    std::string name;
    TaskCaptureSource source = TaskCaptureSource::Unknown;
    AutoType type{};
};

struct TaskCaptureProof {
    std::string name;
    TaskCaptureTransfer transfer = TaskCaptureTransfer::Move;
    Capabilities capabilities{};
};

/**
 * Exact compiler-side admission for one independently scheduled Task boundary.
 *
 * This is deliberately backend/lowering neutral: it proves only that the
 * capture set may cross an independent Task ownership boundary. It does not
 * authorize scheduler publication, continuation execution, scoped borrowing,
 * GPU execution, or LLVM IR generation.
 *
 * Ordinary loans and actor authority fail closed. Owned captures require a
 * derived Send proof from the shared AutoTraitRules lattice. The exact AST
 * boundary identity, ownership-check epoch and execution domain are retained
 * so a reparsed/cloned/stale boundary cannot reuse this admission.
 */
class TaskCaptureAdmission final {
public:
    static TaskCaptureAdmission admit(
            const void* boundary_identity,
            std::uint64_t check_epoch,
            TaskExecutionDomain domain,
            const std::vector<TaskCaptureRequest>& captures) {
        if (boundary_identity == nullptr)
            throw std::invalid_argument("Task admission requires exact boundary identity");
        if (check_epoch == 0)
            throw std::invalid_argument("Task admission requires positive ownership epoch");

        std::unordered_set<std::string> names;
        std::vector<TaskCaptureProof> proofs;
        proofs.reserve(captures.size());

        for (const auto& capture : captures) {
            if (capture.name.empty())
                throw std::invalid_argument("Task capture requires a non-empty lexical name");
            if (!names.insert(capture.name).second)
                throw std::invalid_argument("duplicate Task capture '" + capture.name + "'");

            const Capabilities caps = infer_auto_traits(capture.type);
            switch (capture.source) {
            case TaskCaptureSource::CopyValue:
                if (!caps.send)
                    throw std::invalid_argument(
                        "Copy Task capture lacks derived Send proof: " + capture.name);
                proofs.push_back({capture.name, TaskCaptureTransfer::Copy, caps});
                break;
            case TaskCaptureSource::OwnedValue:
                if (!caps.send)
                    throw std::invalid_argument(
                        "owned Task capture lacks derived Send proof: " + capture.name);
                proofs.push_back({capture.name, TaskCaptureTransfer::Move, caps});
                break;
            case TaskCaptureSource::BorrowShared:
            case TaskCaptureSource::BorrowMut:
                throw std::invalid_argument(
                    "ordinary borrowed Task capture is not independently Send-safe: "
                    + capture.name);
            case TaskCaptureSource::ActorSelf:
                throw std::invalid_argument(
                    "independent Task cannot inherit actor self/state authority: "
                    + capture.name);
            case TaskCaptureSource::Unknown:
                throw std::invalid_argument(
                    "Task capture ownership provenance is unknown: " + capture.name);
            }
        }

        return TaskCaptureAdmission(
            boundary_identity, check_epoch, domain, std::move(proofs));
    }

    void require(
            const void* boundary_identity,
            std::uint64_t check_epoch,
            TaskExecutionDomain domain) const {
        if (boundary_identity_ != boundary_identity)
            throw std::invalid_argument("Task admission belongs to a different AST boundary");
        if (check_epoch_ != check_epoch)
            throw std::invalid_argument("Task admission ownership epoch is stale");
        if (domain_ != domain)
            throw std::invalid_argument("Task admission execution domain mismatch");
    }

    const std::vector<TaskCaptureProof>& captures() const noexcept {
        return captures_;
    }

    std::uint64_t check_epoch() const noexcept { return check_epoch_; }
    TaskExecutionDomain domain() const noexcept { return domain_; }

private:
    TaskCaptureAdmission(
            const void* boundary_identity,
            std::uint64_t check_epoch,
            TaskExecutionDomain domain,
            std::vector<TaskCaptureProof> captures)
        : boundary_identity_(boundary_identity),
          check_epoch_(check_epoch),
          domain_(domain),
          captures_(std::move(captures)) {}

    const void* boundary_identity_;
    std::uint64_t check_epoch_;
    TaskExecutionDomain domain_;
    std::vector<TaskCaptureProof> captures_;
};

} // namespace oreslang::types
