#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <vector>

namespace oreslang::types {

// Compiler-owned affine session typestate CFG proof. Each mask bit denotes
// a possible incoming phase; join is union, not an optimistic selected phase.
// Full source lowering, runtime enforcement, supervisor CONTROL and native
// guest AOT remain separate gates.
enum class NativeSessionPhase : std::uint8_t {
    Unminted, Created, AuthPending, AuthUncertain, Authorized,
    CapturePending, Captured, Closed
};
enum class NativeSessionAction : std::uint8_t {
    Mint, AdmitAuth, AuthTimeout, SettleAuth, AdmitCapture,
    SettleCapture, Close, ReuseCreated, ReuseAuthorized,
    CopyCapability, RebindGeneration
};
struct NativeSessionBlock {
    std::vector<NativeSessionAction> actions;
    std::vector<std::size_t> successors;
};
struct NativeSessionGraph {
    std::uint64_t session_epoch = 0;
    std::uint64_t generation = 0;
    std::vector<NativeSessionBlock> blocks;
};
struct NativeSessionAnalysis {
    std::vector<std::uint16_t> incoming;
    std::vector<std::uint16_t> outgoing;
};
inline constexpr std::uint16_t session_bit(NativeSessionPhase phase) noexcept {
    return static_cast<std::uint16_t>(1u << static_cast<unsigned>(phase));
}
inline NativeSessionPhase require_session_phase(
        NativeSessionAction action, NativeSessionPhase actual,
        NativeSessionPhase expected, NativeSessionPhase next) {
    if (actual != expected) {
        (void)action;
        throw std::invalid_argument("illegal actor session phase at CFG join");
    }
    return next;
}
inline NativeSessionPhase transition_session(
        NativeSessionAction action, NativeSessionPhase phase) {
    using P = NativeSessionPhase;
    using A = NativeSessionAction;
    switch (action) {
    case A::Mint:
        return require_session_phase(action, phase, P::Unminted, P::Created);
    case A::AdmitAuth:
        return require_session_phase(action, phase, P::Created, P::AuthPending);
    case A::AuthTimeout:
        return require_session_phase(action, phase, P::AuthPending, P::AuthUncertain);
    case A::SettleAuth:
        if (phase != P::AuthPending && phase != P::AuthUncertain)
            throw std::invalid_argument("unauthorized session settlement");
        return P::Authorized;
    case A::AdmitCapture:
        return require_session_phase(action, phase, P::Authorized, P::CapturePending);
    case A::SettleCapture:
        return require_session_phase(action, phase, P::CapturePending, P::Captured);
    case A::Close:
        if (phase != P::Created && phase != P::Authorized && phase != P::Captured)
            throw std::invalid_argument("cannot close unsettled protocol work");
        return P::Closed;
    case A::ReuseCreated:
        return require_session_phase(action, phase, P::Created, P::Created);
    case A::ReuseAuthorized:
        return require_session_phase(action, phase, P::Authorized, P::Authorized);
    case A::CopyCapability:
        throw std::invalid_argument("session phase capabilities are affine");
    case A::RebindGeneration:
        throw std::invalid_argument("cross-generation session migration requires separate proof");
    }
    throw std::invalid_argument("unknown session action");
}
inline NativeSessionAnalysis verify_native_session_cfg(const NativeSessionGraph& graph) {
    if (graph.session_epoch == 0 || graph.generation == 0
            || graph.blocks.empty() || graph.blocks.size() > 4096)
        throw std::invalid_argument("missing bounded session/generation identity");
    std::size_t edges = 0;
    for (const auto& block : graph.blocks) {
        edges += block.successors.size();
        if (edges > 16384) throw std::invalid_argument("session CFG edge budget exceeded");
        for (const auto successor : block.successors)
            if (successor >= graph.blocks.size())
                throw std::invalid_argument("session CFG edge points outside graph");
    }
    NativeSessionAnalysis result;
    result.incoming.assign(graph.blocks.size(), 0);
    result.outgoing.assign(graph.blocks.size(), 0);
    result.incoming[0] = session_bit(NativeSessionPhase::Unminted);
    std::deque<std::size_t> todo{0};
    std::vector<bool> queued(graph.blocks.size(), false);
    queued[0] = true;
    while (!todo.empty()) {
        const std::size_t blockId = todo.front();
        todo.pop_front();
        queued[blockId] = false;
        std::uint16_t possible = result.incoming[blockId];
        for (const auto action : graph.blocks[blockId].actions) {
            std::uint16_t next = 0;
            for (unsigned i = 0; i < 8; ++i) {
                if ((possible & (1u << i)) == 0) continue;
                const auto after = transition_session(action,
                        static_cast<NativeSessionPhase>(i));
                next |= session_bit(after);
            }
            possible = next;
        }
        if (possible == result.outgoing[blockId]) continue;
        result.outgoing[blockId] = possible;
        for (const auto successor : graph.blocks[blockId].successors) {
            const std::uint16_t united = result.incoming[successor] | possible;
            if (united != result.incoming[successor]) {
                result.incoming[successor] = united;
                if (!queued[successor]) {
                    todo.push_back(successor);
                    queued[successor] = true;
                }
            }
        }
    }
    return result;
}

} // namespace oreslang::types
