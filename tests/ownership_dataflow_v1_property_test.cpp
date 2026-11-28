// Deterministic host-only metamorphic CFG tests; NOT compiled guest proof.
#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>
using namespace oreslang;
static constexpr std::uint64_t Seed = UINT64_C(0x5eeda770c0ffee11);
static constexpr std::size_t Cases = 128;
struct Rng {
    std::uint64_t state = Seed;
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};
static OwnershipV1Op op(OwnershipV1OpKind kind, std::size_t owner = 0,
                        std::size_t target = OwnershipDataflowV1::NoOwner) {
    return {kind, owner, target};
}
struct Trial {
    OwnershipV1Graph graph;
    std::vector<std::size_t> cleanup;
};
static std::vector<std::size_t> ordered_cleanup(
    const std::vector<std::uint64_t>& acquired) {
    std::vector<std::pair<std::uint64_t, std::size_t>> live;
    for (std::size_t owner = 0; owner < acquired.size(); ++owner)
        if (acquired[owner]) live.emplace_back(acquired[owner], owner);
    std::sort(live.begin(), live.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.first > rhs.first;
    });
    std::vector<std::size_t> result;
    for (const auto& entry : live) result.push_back(entry.second);
    return result;
}
static Trial make_trial(Rng& random) {
    const std::size_t created = 2 + (random.next() % 9);
    Trial t;
    auto& g = t.graph;
    for (std::size_t i = 0; i < created * 2; ++i)
        g.owners.push_back("owner-" + std::to_string(i));
    g.blocks.resize(6);
    std::vector<std::uint64_t> acquired(created * 2, 0);
    for (std::size_t i = 0; i < created; ++i) {
        g.blocks[0].operations.push_back(op(OwnershipV1OpKind::Create, i));
        acquired[i] = i + 1;
    }
    g.blocks[0].successors = {1, 2};
    g.blocks[1].operations = {
        op(OwnershipV1OpKind::BorrowShared),
        op(OwnershipV1OpKind::Use),
        op(OwnershipV1OpKind::EndBorrowShared)
    };
    g.blocks[1].successors = {3};
    g.blocks[2].operations = {
        op(OwnershipV1OpKind::BorrowMutable),
        op(OwnershipV1OpKind::UseThroughMutableBorrow),
        op(OwnershipV1OpKind::EndBorrowMutable)
    };
    g.blocks[2].successors = {3};
    // Keep anchors 0 and 1 live; generated mutations are then well-defined.
    for (std::size_t owner = 2; owner < created; ++owner) {
        switch (random.next() % 4) {
        case 0:
            g.blocks[3].operations.push_back(op(OwnershipV1OpKind::Drop, owner));
            acquired[owner] = 0;
            break;
        case 1: {
            const std::size_t dest = created + owner;
            g.blocks[3].operations.push_back(op(OwnershipV1OpKind::Take, owner, dest));
            acquired[dest] = acquired[owner];
            acquired[owner] = 0;
            break;
        }
        case 2:
            g.blocks[3].operations.push_back(op(OwnershipV1OpKind::Use, owner));
            break;
        default:
            g.blocks[3].operations.push_back(op(OwnershipV1OpKind::UseMutable, owner));
            break;
        }
    }
    g.blocks[3].successors = {4, 5};
    g.blocks[4].operations = {op(OwnershipV1OpKind::ExitNormal)};
    g.blocks[5].operations = {op(OwnershipV1OpKind::ExitNormal)};
    t.cleanup = ordered_cleanup(acquired);
    return t;
}
static bool matches(const OwnershipV1Result& result,
                    const std::vector<std::size_t>& cleanup) {
    if (result.reachable_blocks != 6 || result.analyzed_edges != 6 ||
        result.normal_exits.size() != 2) return false;
    bool seen4 = false, seen5 = false;
    for (const auto& exit : result.normal_exits) {
        if (exit.cleanup_reverse_order != cleanup) return false;
        if (exit.block == 4) seen4 = true;
        else if (exit.block == 5) seen5 = true;
        else return false;
    }
    return seen4 && seen5;
}
static bool rejects(OwnershipV1Graph graph, const char* detail) {
    const std::string expected = std::string("OwnershipDataflowV1: ") + detail;
    try { (void)OwnershipDataflowV1::analyze(graph); }
    catch (const std::runtime_error& error) { return error.what() == expected; }
    return false;
}
static bool check(std::size_t iteration, Rng& random) {
    const Trial t = make_trial(random);
    if (!matches(OwnershipDataflowV1::analyze(t.graph), t.cleanup)) {
        std::fprintf(stderr, "PROPERTY_FAIL|i=%zu|check=cleanup_oracle\n", iteration);
        return false;
    }
    // A branch-local open loan must be rejected exactly at the CFG join.
    auto loan_mutant = t.graph;
    loan_mutant.blocks[1].operations.push_back(op(OwnershipV1OpKind::BorrowShared));
    if (!rejects(std::move(loan_mutant), "ownership states disagree at CFG join")) {
        std::fprintf(stderr, "PROPERTY_FAIL|i=%zu|check=join_loan\n", iteration);
        return false;
    }
    // The anchor cannot be read after explicit destruction.
    auto drop_mutant = t.graph;
    drop_mutant.blocks[3].operations.push_back(op(OwnershipV1OpKind::Drop));
    drop_mutant.blocks[3].operations.push_back(op(OwnershipV1OpKind::Use));
    if (!rejects(std::move(drop_mutant), "use requires a live owned value")) {
        std::fprintf(stderr, "PROPERTY_FAIL|i=%zu|check=use_after_drop\n", iteration);
        return false;
    }
    // Changing the relative order of two surviving Creates changes the
    // destructor order exactly as predicted by the construction oracle.
    auto order_mutant = t.graph;
    std::swap(order_mutant.blocks[0].operations[0],
              order_mutant.blocks[0].operations[1]);
    auto expected = t.cleanup;
    const auto a = std::find(expected.begin(), expected.end(), 0);
    const auto b = std::find(expected.begin(), expected.end(), 1);
    if (a == expected.end() || b == expected.end()) return false;
    std::iter_swap(a, b);
    if (expected == t.cleanup ||
        !matches(OwnershipDataflowV1::analyze(order_mutant), expected)) {
        std::fprintf(stderr, "PROPERTY_FAIL|i=%zu|check=acquisition_order\n", iteration);
        return false;
    }
    return true;
}
int main() {
    Rng rng;
    std::size_t failed = 0;
    for (std::size_t i = 0; i < Cases; ++i) {
        bool passed = false;
        try { passed = check(i, rng); }
        catch (const std::exception& e) {
            std::fprintf(stderr, "PROPERTY_EXCEPTION|i=%zu|what=%s\n", i, e.what());
        } catch (...) {
            std::fprintf(stderr, "PROPERTY_EXCEPTION|i=%zu|what=unknown\n", i);
        }
        if (!passed) ++failed;
    }
    std::printf(
        "OWNERSHIP_CFG_PROPERTY|seed=0x%llx|cases=%zu|checks=%zu|failed=%zu|result=%s\n",
        static_cast<unsigned long long>(Seed), Cases, Cases * 4, failed,
        failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
