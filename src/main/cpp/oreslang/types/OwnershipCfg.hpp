#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::ownership {

// Powerset lattice: a join preserves *every* possible incoming state.
// In particular {Owned, Moved} is NOT definitely initialized or movable.
using ValueMask = std::uint8_t;
constexpr ValueMask kUninitialized = 1;
constexpr ValueMask kOwned = 2;
constexpr ValueMask kMoved = 4;
constexpr ValueMask kDropped = 8;

struct PlaceState {
    ValueMask possible = kUninitialized;
    unsigned shared_loans = 0;
    bool exclusive_loan = false;
    // Identity, not merely the number of borrows, must survive a CFG join.
    // Token zero is reserved for a legacy loan already live at graph entry.
    std::set<std::uint64_t> shared_loan_ids;
    std::optional<std::uint64_t> exclusive_loan_id;

    bool operator==(const PlaceState& rhs) const {
        return possible == rhs.possible &&
            shared_loans == rhs.shared_loans &&
            exclusive_loan == rhs.exclusive_loan &&
            shared_loan_ids == rhs.shared_loan_ids &&
            exclusive_loan_id == rhs.exclusive_loan_id;
    }
};
using State = std::map<std::string, PlaceState>;

struct Operation {
    enum class Kind {
        Initialize, Read, Move, Drop, DropIfOwned,
        ReadObject, MoveObject, BorrowShared, BorrowExclusive,
        ReleaseShared, ReleaseExclusive
    };
    Kind kind;
    std::string place;
    // Optional explicit lexical loan identity; absent borrow IDs derive from
    // the original CFG operation site, not the active traversal order.
    std::optional<std::uint64_t> loan_id;
};

struct BasicBlock {
    std::vector<Operation> operations;
    std::vector<std::size_t> successors;
};

struct Graph {
    State entry;
    std::vector<BasicBlock> blocks;
};

struct Analysis {
    std::vector<std::optional<State>> incoming;
    std::vector<std::optional<State>> outgoing;
};

[[noreturn]] inline void violation(const std::string& name, const std::string& why) {
    throw std::runtime_error("ownership violation: " + why + " '" + name + "'");
}

inline PlaceState& place(State& state, const std::string& key) {
    const auto found = state.find(key);
    if (found == state.end())
        violation(key, "unknown CFG place");
    return found->second;
}

inline void require_owned(const std::string& key, const PlaceState& value) {
    if (value.possible != kOwned)
        violation(key, "not definitely initialized on every CFG predecessor");
}

inline void apply(Operation op, State& state, std::uint64_t site_id = 0) {
    if (op.kind == Operation::Kind::ReadObject ||
            op.kind == Operation::Kind::MoveObject) {
        // A non-Copy object requires *all* its fields. A moved field does
        // not prevent reading a different owned field, but prevents moving
        // or borrowing the complete object again.
        const std::string prefix = op.place + ".";
        bool saw_field = false;
        for (auto& item : state) {
            if (item.first.compare(0, prefix.size(), prefix) != 0) continue;
            saw_field = true;
            require_owned(item.first, item.second);
            // Reading an object aliases every field. An active exclusive
            // field loan forbids a whole-object read; shared field loans
            // permit reads but prohibit moving/dropping the complete owner.
            if (op.kind == Operation::Kind::ReadObject &&
                    item.second.exclusive_loan)
                violation(op.place, "whole-object read overlaps exclusive field loan");
            if (op.kind == Operation::Kind::MoveObject &&
                    (item.second.shared_loans || item.second.exclusive_loan))
                violation(op.place, "move while any field is borrowed");
        }
        if (!saw_field) violation(op.place, "object has no tracked fields");
        if (op.kind == Operation::Kind::MoveObject)
            for (auto& item : state)
                if (item.first.compare(0, prefix.size(), prefix) == 0)
                    item.second.possible = kMoved;
        return;
    }
    PlaceState& value = place(state, op.place);
    switch (op.kind) {
        case Operation::Kind::Initialize:
            if (value.possible != kUninitialized || value.shared_loans || value.exclusive_loan)
                violation(op.place, "initializing a live or ambiguous owner");
            value.possible = kOwned;
            return;
        case Operation::Kind::Read:
            require_owned(op.place, value);
            if (value.exclusive_loan) violation(op.place, "read overlaps mutable loan");
            return;
        case Operation::Kind::Move:
        case Operation::Kind::Drop:
            require_owned(op.place, value);
            if (value.shared_loans || value.exclusive_loan)
                violation(op.place, "moving/dropping a borrowed owner");
            value.possible = op.kind == Operation::Kind::Move ? kMoved : kDropped;
            return;
        case Operation::Kind::DropIfOwned:
            if (value.shared_loans || value.exclusive_loan)
                violation(op.place, "conditional drop with a live loan");
            // Runtime lowering MUST generate a corresponding ownership flag.
            if (value.possible & kOwned)
                value.possible = static_cast<ValueMask>(
                    (value.possible & static_cast<ValueMask>(~kOwned)) | kDropped);
            return;
        case Operation::Kind::BorrowShared:
            require_owned(op.place, value);
            if (value.exclusive_loan) violation(op.place, "shared borrow overlaps mutable loan");
            if (value.shared_loans == 65535) violation(op.place, "shared loan count exceeds bound");
            if (!value.shared_loan_ids.insert(op.loan_id.value_or(site_id)).second)
                violation(op.place, "shared loan identity already live");
            ++value.shared_loans;
            return;
        case Operation::Kind::BorrowExclusive:
            require_owned(op.place, value);
            if (value.exclusive_loan || value.shared_loans)
                violation(op.place, "overlapping exclusive loans");
            value.exclusive_loan = true;
            value.exclusive_loan_id = op.loan_id.value_or(site_id);
            return;
        case Operation::Kind::ReleaseShared:
            if (!value.shared_loans) violation(op.place, "unmatched shared loan release");
            if (!op.loan_id && value.shared_loan_ids.size() != 1)
                violation(op.place, "shared loan release requires unambiguous provenance");
            const auto to_release = op.loan_id.value_or(*value.shared_loan_ids.begin());
            if (!value.shared_loan_ids.erase(to_release))
                violation(op.place, "shared loan release has foreign provenance");
            --value.shared_loans;
            return;
        case Operation::Kind::ReleaseExclusive:
            if (!value.exclusive_loan) violation(op.place, "unmatched exclusive loan release");
            if (op.loan_id && op.loan_id != value.exclusive_loan_id)
                violation(op.place, "exclusive loan release has foreign provenance");
            value.exclusive_loan = false;
            value.exclusive_loan_id.reset();
            return;
        case Operation::Kind::ReadObject:
        case Operation::Kind::MoveObject:
            break;
    }
    violation(op.place, "invalid CFG operation");
}

inline bool join_into(State& target, const State& incoming) {
    if (target.size() != incoming.size())
        throw std::runtime_error("ownership violation: inconsistent CFG place sets");
    bool changed = false;
    for (auto& pair : target) {
        const auto other = incoming.find(pair.first);
        if (other == incoming.end())
            violation(pair.first, "missing CFG predecessor place");
        // Do not invent a released loan by joining distinct loan histories.
        // A future provenance-aware pass may make this more precise.
        if (pair.second.shared_loans != other->second.shared_loans ||
                pair.second.exclusive_loan != other->second.exclusive_loan ||
                pair.second.shared_loan_ids != other->second.shared_loan_ids ||
                pair.second.exclusive_loan_id != other->second.exclusive_loan_id)
            violation(pair.first, "different live loan provenance at CFG join");
        ValueMask merged = static_cast<ValueMask>(pair.second.possible | other->second.possible);
        if (merged != pair.second.possible) {
            pair.second.possible = merged;
            changed = true;
        }
    }
    return changed;
}

inline Analysis verify(const Graph& graph) {
    if (graph.blocks.empty())
        throw std::runtime_error("ownership violation: empty CFG");
    // Reject malformed initial abstract states before any CFG edge is
    // processed. Unknown bits and contradictory live-loan provenance
    // must not silently enter the fixed-point lattice.
    State entry = graph.entry;
    for (auto& [name, place] : entry) {
        if (!place.possible || (place.possible &
                static_cast<ValueMask>(~(kOwned | kMoved | kUninitialized | kDropped))))
            violation(name, "invalid CFG entry ownership mask");
        if (place.exclusive_loan && place.shared_loans)
            violation(name, "conflicting initial borrow states");
        // A live loan must refer to a definitely owned place, not a
        // possibly moved/dropped/uninitialized predecessor. Otherwise a
        // matching Release can erase the impossible loan and admit a path.
        if ((place.exclusive_loan || place.shared_loans) &&
                place.possible != kOwned)
            violation(name, "live loan on non-owned CFG entry place");
        if (place.shared_loans > 65535)
            violation(name, "shared loan count exceeds bound");
        // Backwards-compatible entry loan identity: an entry with one live
        // loan can be named unambiguously; multiple anonymous entry loans
        // would make release ownership impossible to prove.
        if (place.shared_loans == 1 && place.shared_loan_ids.empty())
            place.shared_loan_ids.insert(0);
        if (place.exclusive_loan && !place.exclusive_loan_id)
            place.exclusive_loan_id = 0;
        if (place.shared_loans != place.shared_loan_ids.size() ||
                place.exclusive_loan != place.exclusive_loan_id.has_value())
            violation(name, "unproven initial loan identity");
    }
    Analysis result;
    result.incoming.resize(graph.blocks.size());
    result.outgoing.resize(graph.blocks.size());
    result.incoming[0] = entry;
    std::deque<std::size_t> queue{0};
    std::vector<bool> queued(graph.blocks.size(), false);
    queued[0] = true;
    while (!queue.empty()) {
        const auto index = queue.front();
        queue.pop_front();
        queued[index] = false;
        State current = *result.incoming[index];
        const BasicBlock& block = graph.blocks[index];
        for (std::size_t site = 0; site < block.operations.size(); ++site) {
            // Reserve zero for an explicitly known loan at entry. Source
            // position gives stable IDs across fixed-point reprocessing.
            if (index >= UINT32_MAX || site >= UINT32_MAX)
                throw std::runtime_error("ownership violation: CFG too large for loan identity");
            const auto site_id = ((static_cast<std::uint64_t>(index) + 1) << 32)
                    | (static_cast<std::uint64_t>(site) + 1);
            apply(block.operations[site], current, site_id);
        }
        if (block.successors.empty())
            for (const auto& item : current)
                if (item.second.shared_loans || item.second.exclusive_loan)
                    violation(item.first, "loan escapes CFG exit");
        result.outgoing[index] = current;
        for (std::size_t next : block.successors) {
            if (next >= graph.blocks.size())
                throw std::runtime_error("ownership violation: CFG edge outside graph");
            bool changed = false;
            if (!result.incoming[next]) {
                result.incoming[next] = current;
                changed = true;
            } else {
                changed = join_into(*result.incoming[next], current);
            }
            if (changed && !queued[next]) {
                queue.push_back(next);
                queued[next] = true;
            }
        }
    }
    return result;
}

} // namespace oreslang::ownership
