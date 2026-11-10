#pragma once

// OwnershipDataflowV1 is a bounded, conservative CFG verifier. It performs
// no allocation at runtime and is intended for compiler-time use only.
// Unknown exits and disagreeing join states fail closed; no tracing-GC or
// implicit ownership repair is synthesized to hide an invalid program.
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace oreslang {

enum class OwnershipV1OpKind : std::uint8_t {
    Create,
    Use,
    UseMutable,
    UseThroughMutableBorrow,
    BorrowShared,
    BorrowMutable,
    EndBorrowShared,
    EndBorrowMutable,
    Take,
    Drop,
    ExitNormal,
    ExitExceptional,
    ExitSuspend,
    ExitCancel
};

struct OwnershipV1Op {
    OwnershipV1OpKind kind;
    std::size_t owner = 0;
    // Take transfers the owner's resource into a distinct, previously
    // uninitialized destination. SIZE_MAX means no destination and is invalid.
    std::size_t target = static_cast<std::size_t>(-1);
};

struct OwnershipV1Block {
    std::vector<OwnershipV1Op> operations;
    std::vector<std::size_t> successors;
};

struct OwnershipV1Graph {
    std::vector<std::string> owners;
    std::vector<OwnershipV1Block> blocks;
    std::size_t entry = 0;
};

struct OwnershipV1ExitPlan {
    std::size_t block = 0;
    std::vector<std::size_t> cleanup_reverse_order;
};

struct OwnershipV1Result {
    std::vector<OwnershipV1ExitPlan> normal_exits;
    std::size_t reachable_blocks = 0;
    std::size_t analyzed_edges = 0;
};

class OwnershipDataflowV1 final {
public:
    // Optional same-pass observer. Each successful operation is reported from
    // the *existing* typed CFG verifier, without a second graph traversal.
    // Shadow events are not independently authenticated borrow proofs.
    using ShadowSinkV0 = void (*)(void* context, std::size_t block,
                                  std::size_t operation,
                                  OwnershipV1OpKind kind, std::size_t owner);
    static constexpr std::size_t NoOwner = static_cast<std::size_t>(-1);
    static constexpr std::size_t MaxOwners = 64;
    static constexpr std::size_t MaxBlocks = 4096;
    static constexpr std::size_t MaxEdges = 16384;
    static constexpr std::size_t MaxOperationsPerBlock = 1024;
    static constexpr std::size_t MaxBorrowsPerOwner = 65535;

    static OwnershipV1Result analyze(const OwnershipV1Graph& graph,
                                     ShadowSinkV0 shadow = nullptr,
                                     void* shadow_context = nullptr) {
        if (graph.owners.empty() || graph.owners.size() > MaxOwners)
            fail("owner count outside bounded analysis limit");
        if (graph.blocks.empty() || graph.blocks.size() > MaxBlocks ||
            graph.entry >= graph.blocks.size())
            fail("invalid or unbounded CFG");
        for (std::size_t i = 0; i < graph.owners.size(); ++i) {
            if (graph.owners[i].empty()) fail("empty owner identity");
            for (std::size_t j = 0; j < i; ++j)
                if (graph.owners[i] == graph.owners[j])
                    fail("duplicate owner identity");
        }

        std::size_t edge_count = 0;
        for (const auto& block : graph.blocks) {
            if (block.operations.size() > MaxOperationsPerBlock)
                fail("operation budget exceeded");
            if (block.successors.size() > MaxEdges - edge_count)
                fail("edge budget exceeded");
            edge_count += block.successors.size();
            for (const auto next : block.successors)
                if (next >= graph.blocks.size()) fail("invalid CFG successor");
        }

        State initial(graph.owners.size());
        std::vector<bool> reached(graph.blocks.size(), false);
        std::vector<State> incoming(graph.blocks.size());
        std::deque<std::size_t> work;
        reached[graph.entry] = true;
        incoming[graph.entry] = initial;
        work.push_back(graph.entry);
        OwnershipV1Result result;
        std::vector<bool> exit_seen(graph.blocks.size(), false);

        while (!work.empty()) {
            const auto index = work.front();
            work.pop_front();
            const auto& block = graph.blocks[index];
            State state = incoming[index];
            ++result.reachable_blocks;

            bool has_exit = false;
            std::uint64_t next_acquisition_order = 1;
            // Acquisition sequence is path state, not a per-block counter.
            // Seed it from the maximum sequence already present at entry.
            for (const auto& owner : state)
                if (owner.acquisition_order >= next_acquisition_order)
                    next_acquisition_order = owner.acquisition_order + 1;
            for (std::size_t op_index = 0; op_index < block.operations.size(); ++op_index) {
                const auto& op = block.operations[op_index];
                if (op.kind == OwnershipV1OpKind::ExitNormal ||
                    op.kind == OwnershipV1OpKind::ExitExceptional ||
                    op.kind == OwnershipV1OpKind::ExitSuspend ||
                    op.kind == OwnershipV1OpKind::ExitCancel) {
                    if (op_index + 1 != block.operations.size())
                        fail("exit operation must terminate its basic block");
                    if (op.owner != 0 || op.target != NoOwner)
                        fail("exit operation must not carry owner or transfer metadata");
                    if (!block.successors.empty())
                        fail("exit block must not have successors");
                    if (op.kind != OwnershipV1OpKind::ExitNormal)
                        fail("exceptional, suspension and cancellation exits are not admitted yet");
                    if (exit_seen[index]) fail("duplicate exit processing");
                    exit_seen[index] = true;
                    OwnershipV1ExitPlan plan;
                    plan.block = index;
                    for (std::size_t owner = 0; owner < state.size(); ++owner) {
                        const auto& item = state[owner];
                        if (item.shared_borrows || item.mutable_borrow)
                            fail("normal exit with outstanding borrow");
                        if (item.state == OwnerState::Owned)
                            plan.cleanup_reverse_order.push_back(owner);
                    }
                    std::stable_sort(plan.cleanup_reverse_order.begin(),
                        plan.cleanup_reverse_order.end(),
                        [&](std::size_t lhs, std::size_t rhs) {
                            return state[lhs].acquisition_order >
                                   state[rhs].acquisition_order;
                        });
                    result.normal_exits.push_back(std::move(plan));
                    if (shadow != nullptr)
                        shadow(shadow_context, index, op_index, op.kind, op.owner);
                    has_exit = true;
                    break;
                }
                apply(op, state, graph.owners.size(), next_acquisition_order);
                if (shadow != nullptr)
                    shadow(shadow_context, index, op_index, op.kind, op.owner);
            }
            if (has_exit) continue;
            if (block.successors.empty())
                fail("reachable CFG block has no explicit exit");
            for (const auto successor : block.successors) {
                ++result.analyzed_edges;
                if (!reached[successor]) {
                    reached[successor] = true;
                    incoming[successor] = state;
                    work.push_back(successor);
                } else if (!(incoming[successor] == state)) {
                    // Conservative SSA-style join: the owner and borrow state
                    // must be identical on all incoming paths. A future PHI/
                    // flag-sensitive analysis may relax this with proofs.
                    fail("ownership states disagree at CFG join");
                }
            }
        }
        if (result.normal_exits.empty()) fail("CFG has no reachable normal exit");
        for (std::size_t i = 0; i < reached.size(); ++i)
            if (!reached[i])
                fail("unreachable CFG block is not admitted in v1");

        // V1 is intentionally a terminating acyclic CFG subset. Merely
        // reaching one exit does not prove that a second path cannot loop
        // forever while retaining an owned region. Loops need a separate
        // bounded-iteration/liveness proof before they can be admitted.
        std::vector<std::size_t> indegree(graph.blocks.size(), 0);
        std::size_t reachable_count = 0;
        for (std::size_t from = 0; from < graph.blocks.size(); ++from) {
            if (!reached[from]) continue;
            ++reachable_count;
            for (const auto to : graph.blocks[from].successors)
                if (reached[to]) ++indegree[to];
        }
        std::deque<std::size_t> ready;
        for (std::size_t i = 0; i < graph.blocks.size(); ++i)
            if (reached[i] && indegree[i] == 0) ready.push_back(i);
        std::size_t topological_count = 0;
        while (!ready.empty()) {
            const auto from = ready.front();
            ready.pop_front();
            ++topological_count;
            for (const auto to : graph.blocks[from].successors)
                if (reached[to] && --indegree[to] == 0) ready.push_back(to);
        }
        if (topological_count != reachable_count)
            fail("cyclic CFG requires a separate bounded-loop ownership proof");
        return result;
    }

private:
    enum class OwnerState : std::uint8_t { Uninitialized, Owned, Moved, Destroyed };
    struct Owner {
        OwnerState state = OwnerState::Uninitialized;
        std::uint32_t shared_borrows = 0;
        bool mutable_borrow = false;
        std::uint64_t acquisition_order = 0;
        bool operator==(const Owner& other) const noexcept {
            return state == other.state && shared_borrows == other.shared_borrows &&
                   mutable_borrow == other.mutable_borrow &&
                   acquisition_order == other.acquisition_order;
        }
    };
    using State = std::vector<Owner>;

    [[noreturn]] static void fail(const char* message) {
        throw std::runtime_error(std::string("OwnershipDataflowV1: ") + message);
    }

    static Owner& get(State& state, std::size_t owner, std::size_t count) {
        if (owner >= count) fail("operation references unknown owner");
        return state[owner];
    }

    static void require_owned(const Owner& owner) {
        if (owner.state != OwnerState::Owned)
            fail("use requires a live owned value");
    }

    static void apply(const OwnershipV1Op& op, State& state, std::size_t count,
                      std::uint64_t& next_acquisition_order) {
        if (op.kind != OwnershipV1OpKind::Take &&
            op.target != NoOwner)
            fail("unexpected transfer target on non-take operation");
        Owner& owner = get(state, op.owner, count);
        switch (op.kind) {
            case OwnershipV1OpKind::Create:
                if (owner.state != OwnerState::Uninitialized)
                    fail("create on initialized owner");
                if (next_acquisition_order == UINT64_MAX)
                    fail("owner acquisition sequence exhausted");
                owner.state = OwnerState::Owned;
                owner.acquisition_order = next_acquisition_order++;
                break;
            case OwnershipV1OpKind::Use:
                require_owned(owner);
                if (owner.mutable_borrow) fail("read conflicts with mutable borrow");
                break;
            case OwnershipV1OpKind::UseMutable:
                require_owned(owner);
                if (owner.shared_borrows || owner.mutable_borrow)
                    fail("mutable access conflicts with outstanding borrow");
                break;
            case OwnershipV1OpKind::UseThroughMutableBorrow:
                require_owned(owner);
                if (!owner.mutable_borrow || owner.shared_borrows)
                    fail("mutable-borrow access requires its exclusive loan");
                break;
            case OwnershipV1OpKind::BorrowShared:
                require_owned(owner);
                if (owner.mutable_borrow || owner.shared_borrows == MaxBorrowsPerOwner)
                    fail("shared borrow conflicts or exceeds quota");
                ++owner.shared_borrows;
                break;
            case OwnershipV1OpKind::BorrowMutable:
                require_owned(owner);
                if (owner.mutable_borrow || owner.shared_borrows)
                    fail("mutable borrow requires exclusive access");
                owner.mutable_borrow = true;
                break;
            case OwnershipV1OpKind::EndBorrowShared:
                if (owner.state != OwnerState::Owned || owner.shared_borrows == 0)
                    fail("shared borrow end without matching borrow");
                --owner.shared_borrows;
                break;
            case OwnershipV1OpKind::EndBorrowMutable:
                if (owner.state != OwnerState::Owned || !owner.mutable_borrow)
                    fail("mutable borrow end without matching borrow");
                owner.mutable_borrow = false;
                break;
            case OwnershipV1OpKind::Take: {
                require_owned(owner);
                if (owner.shared_borrows || owner.mutable_borrow)
                    fail("take while borrow is outstanding");
                if (op.target >= count || op.target == op.owner)
                    fail("take requires a distinct valid destination owner");
                Owner& destination = get(state, op.target, count);
                if (destination.state != OwnerState::Uninitialized)
                    fail("take destination is already initialized");
                destination.state = OwnerState::Owned;
                destination.acquisition_order = owner.acquisition_order;
                // Retain the historical sequence on the moved source slot.
                // The destination inherits this same resource identity; keeping
                // history prevents later allocations from reusing an old order.
                owner.state = OwnerState::Moved;
                break;
            }
            case OwnershipV1OpKind::Drop:
                require_owned(owner);
                if (owner.shared_borrows || owner.mutable_borrow)
                    fail("drop while borrow is outstanding");
                // Keep acquisition history after destruction. A subsequent
                // allocation on this path must receive a strictly newer order.
                owner.state = OwnerState::Destroyed;
                break;
            case OwnershipV1OpKind::ExitNormal:
            case OwnershipV1OpKind::ExitExceptional:
            case OwnershipV1OpKind::ExitSuspend:
            case OwnershipV1OpKind::ExitCancel:
                fail("exit must be handled by CFG walker");
            default:
                fail("unknown ownership operation kind");
        }
    }
};

} // namespace oreslang
