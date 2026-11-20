#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return false; } } while (0)

static OwnershipV1Op op(OwnershipV1OpKind kind, std::size_t owner = 0,
                         std::size_t target = static_cast<std::size_t>(-1)) {
    return {kind, owner, target};
}
static OwnershipV1Graph graph(std::size_t owners, std::size_t blocks = 1) {
    OwnershipV1Graph g;
    for (std::size_t i = 0; i < owners; ++i) g.owners.push_back("owner-" + std::to_string(i));
    g.blocks.resize(blocks);
    return g;
}
static bool normal_cleanup_reverse_order() {
    auto g = graph(3, 3);
    g.blocks[0].operations = {op(OwnershipV1OpKind::Create,0),op(OwnershipV1OpKind::Create,1),op(OwnershipV1OpKind::Create,2)};
    g.blocks[0].successors = {1,2};
    g.blocks[1].operations = {op(OwnershipV1OpKind::ExitNormal)};
    g.blocks[2].operations = {op(OwnershipV1OpKind::ExitNormal)};
    auto r = OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 2);
    for (const auto& exit : r.normal_exits) {
        CHECK(exit.cleanup_reverse_order.size() == 3);
        CHECK(exit.cleanup_reverse_order[0] == 2);
        CHECK(exit.cleanup_reverse_order[1] == 1);
        CHECK(exit.cleanup_reverse_order[2] == 0);
    }
    return true;
}
static bool take_transfers_to_destination() {
    auto g = graph(3);
    g.blocks[0].operations = {
        op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1),
        op(OwnershipV1OpKind::Take,1,2),
        op(OwnershipV1OpKind::ExitNormal)};
    auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size()==1);
    CHECK(r.normal_exits[0].cleanup_reverse_order.size()==2);
    CHECK(r.normal_exits[0].cleanup_reverse_order[0]==2);
    CHECK(r.normal_exits[0].cleanup_reverse_order[1]==0);
    return true;
}
static bool explicit_drop_not_repeated() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::ExitNormal)};
    auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits[0].cleanup_reverse_order.empty());
    return true;
}
static bool shared_borrow_release() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::Use),op(OwnershipV1OpKind::EndBorrowShared),op(OwnershipV1OpKind::ExitNormal)};
    return OwnershipDataflowV1::analyze(g).normal_exits[0].cleanup_reverse_order.size()==1;
}
static bool mutable_borrow_release() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowMutable),op(OwnershipV1OpKind::EndBorrowMutable),op(OwnershipV1OpKind::Use),op(OwnershipV1OpKind::ExitNormal)};
    return OwnershipDataflowV1::analyze(g).normal_exits[0].cleanup_reverse_order.size()==1;
}
static bool mutable_borrow_allows_exclusive_write() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::BorrowMutable),
        op(OwnershipV1OpKind::UseThroughMutableBorrow),
        op(OwnershipV1OpKind::EndBorrowMutable),
        op(OwnershipV1OpKind::ExitNormal)};
    return OwnershipDataflowV1::analyze(g).normal_exits[0].cleanup_reverse_order.size()==1;
}
// Reject for the intended semantic reason, not an earlier malformed fixture.
static bool expect_reject(const char* name, OwnershipV1Graph g,
                          const char* expected_detail) {
    const std::string expected =
        std::string("OwnershipDataflowV1: ") + expected_detail;
    try { (void)OwnershipDataflowV1::analyze(g); }
    catch (const std::runtime_error& error) {
        if (expected == error.what()) return true;
        std::fprintf(stderr,
            "FAIL wrong rejection for %s: expected '%s', got '%s'\n",
            name, expected.c_str(), error.what());
        return false;
    }
    std::fprintf(stderr, "FAIL expected rejection for %s: '%s'\n",
                 name, expected.c_str());
    return false;
}
static bool rejects_double_drop() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("double drop",std::move(g),
        "use requires a live owned value");
}
static bool rejects_use_after_take() {
    auto g=graph(2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Take,0,1),
        op(OwnershipV1OpKind::Use,0),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("use after take",std::move(g),
        "use requires a live owned value");
}
static bool rejects_borrowed_drop() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("drop with borrow",std::move(g),
        "drop while borrow is outstanding");
}
static bool rejects_mutable_alias() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::BorrowMutable),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("shared + mut borrow",std::move(g),
        "mutable borrow requires exclusive access");
}
static bool rejects_join_mismatch() {
    auto g=graph(2,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0)};
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::Take,0,1)};
    g.blocks[1].successors={3};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("join state mismatch",std::move(g),
        "ownership states disagree at CFG join");
}
static bool rejects_unmodeled_exit() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::ExitSuspend)};
    return expect_reject("suspension exit",std::move(g),
        "exceptional, suspension and cancellation exits are not admitted yet");
}
static bool rejects_implicit_exit() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    return expect_reject("missing explicit exit",std::move(g),
        "reachable CFG block has no explicit exit");
}
static bool rejects_duplicate_identity() {
    auto g=graph(2);g.owners[1]=g.owners[0];
    g.blocks[0].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("duplicate owner",std::move(g),
        "duplicate owner identity");
}
static bool rejects_take_without_destination() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::Take),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("take without destination",std::move(g),
        "take requires a distinct valid destination owner");
}
static bool rejects_take_into_live_destination() {
    auto g=graph(2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1),op(OwnershipV1OpKind::Take,0,1),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("take into live destination",std::move(g),
        "take destination is already initialized");
}
static bool rejects_use_before_create() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Use),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("use before create",std::move(g),
        "use requires a live owned value");
}
static bool rejects_unknown_operation() {
    auto g=graph(1);
    g.blocks[0].operations={{static_cast<OwnershipV1OpKind>(255),0},
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("unknown operation kind",std::move(g),
        "unknown ownership operation kind");
}
static bool rejects_unreachable_block() {
    auto g=graph(1,2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitNormal)};
    g.blocks[1].operations={{static_cast<OwnershipV1OpKind>(255),0}};
    return expect_reject("unreachable hidden block",std::move(g),
        "unreachable CFG block is not admitted in v1");
}
static bool rejects_mutation_during_shared_borrow() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::BorrowShared),
        op(OwnershipV1OpKind::UseMutable),
        op(OwnershipV1OpKind::EndBorrowShared),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("mutable use with shared borrow",std::move(g),
        "mutable access conflicts with outstanding borrow");
}
static bool rejects_unbalanced_borrow() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("unbalanced shared borrow",std::move(g),
        "normal exit with outstanding borrow");
}
static bool cleanup_uses_acquisition_not_declaration_order() {
    auto g=graph(3);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,2),
        op(OwnershipV1OpKind::Create,0),op(OwnershipV1OpKind::Create,1),
        op(OwnershipV1OpKind::ExitNormal)};
    auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits[0].cleanup_reverse_order.size()==3);
    CHECK(r.normal_exits[0].cleanup_reverse_order[0]==1);
    CHECK(r.normal_exits[0].cleanup_reverse_order[1]==0);
    CHECK(r.normal_exits[0].cleanup_reverse_order[2]==2);
    return true;
}
static bool rejects_different_acquisition_order_at_join() {
    auto g=graph(2,4);
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1)};
    g.blocks[1].successors={3};
    g.blocks[2].operations={op(OwnershipV1OpKind::Create,1),
        op(OwnershipV1OpKind::Create,0)};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("different acquisition order at join",std::move(g),
        "ownership states disagree at CFG join");
}
static bool rejects_exit_with_owner_metadata() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitNormal,0,0)};
    return expect_reject("exit with transfer metadata",std::move(g),
        "exit operation must not carry owner or transfer metadata");
}
static bool rejects_stray_transfer_target() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::Drop,0,9),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("stray transfer target",std::move(g),
        "unexpected transfer target on non-take operation");
}
static bool rejects_read_during_mutable_borrow() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::BorrowMutable),
        op(OwnershipV1OpKind::Use),
        op(OwnershipV1OpKind::EndBorrowMutable),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("read via owner during mutable borrow",std::move(g),
        "read conflicts with mutable borrow");
}
static bool acquisition_history_survives_move_and_drop() {
    auto g=graph(4);
    g.blocks[0].operations={
        op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1),
        op(OwnershipV1OpKind::Take,0,2),
        op(OwnershipV1OpKind::Drop,1),
        op(OwnershipV1OpKind::Create,3),
        op(OwnershipV1OpKind::ExitNormal)};
    auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size()==1);
    CHECK(r.normal_exits[0].cleanup_reverse_order.size()==2);
    CHECK(r.normal_exits[0].cleanup_reverse_order[0]==3);
    CHECK(r.normal_exits[0].cleanup_reverse_order[1]==2);
    return true;
}
static bool rejects_end_borrow_without_loan() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::EndBorrowShared),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("end missing shared loan",std::move(g),
        "shared borrow end without matching borrow");
}
static bool rejects_unbounded_cycle() {
    auto g=graph(1,3);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks[0].successors={1,2};
    g.blocks[1].successors={1};
    g.blocks[2].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("unbounded ownership cycle",std::move(g),
        "cyclic CFG requires a separate bounded-loop ownership proof");
}

static bool rejects_exceptional_exit() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitExceptional)};
    return expect_reject("exceptional exit",std::move(g),
        "exceptional, suspension and cancellation exits are not admitted yet");
}
static bool rejects_cancellation_exit() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitCancel)};
    return expect_reject("cancellation exit",std::move(g),
        "exceptional, suspension and cancellation exits are not admitted yet");
}
static bool rejects_take_with_outstanding_shared_borrow() {
    auto g=graph(2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::BorrowShared,0),
        op(OwnershipV1OpKind::Take,0,1),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("take with shared borrow",std::move(g),
        "take while borrow is outstanding");
}
static bool rejects_write_through_missing_mutable_loan() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::UseThroughMutableBorrow),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("write without mutable loan",std::move(g),
        "mutable-borrow access requires its exclusive loan");
}
static bool rejects_end_missing_mutable_loan() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::EndBorrowMutable),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("end missing mutable loan",std::move(g),
        "mutable borrow end without matching borrow");
}


static bool join_identical_ownership_states() {
    auto g=graph(2,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1)};
    g.blocks[0].successors={1,2};
    g.blocks[1].successors={3};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.reachable_blocks == 4);
    CHECK(r.analyzed_edges == 4);
    CHECK(r.normal_exits.size() == 1);
    CHECK(r.normal_exits[0].cleanup_reverse_order ==
          std::vector<std::size_t>({1,0}));
    return true;
}
static bool borrowed_then_released_diamond_join() {
    auto g=graph(1,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::BorrowShared),
        op(OwnershipV1OpKind::EndBorrowShared)};
    g.blocks[1].successors={3};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 1);
    CHECK(r.normal_exits[0].cleanup_reverse_order ==
          std::vector<std::size_t>({0}));
    return true;
}
static bool both_branches_drop_before_join() {
    auto g=graph(2,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1)};
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::Drop,1)};
    g.blocks[1].successors={3};
    g.blocks[2].operations={op(OwnershipV1OpKind::Drop,1)};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 1);
    CHECK(r.normal_exits[0].cleanup_reverse_order ==
          std::vector<std::size_t>({0}));
    return true;
}
static bool take_acquisition_order_across_blocks() {
    auto g=graph(3,3);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Create,1)};
    g.blocks[0].successors={1};
    g.blocks[1].operations={op(OwnershipV1OpKind::Take,1,2)};
    g.blocks[1].successors={2};
    g.blocks[2].operations={op(OwnershipV1OpKind::ExitNormal)};
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 1);
    CHECK(r.normal_exits[0].cleanup_reverse_order ==
          std::vector<std::size_t>({2,0}));
    return true;
}
static bool accepts_max_owners() {
    auto g=graph(OwnershipDataflowV1::MaxOwners);
    for (std::size_t i=0; i<g.owners.size(); ++i)
        g.blocks[0].operations.push_back(op(OwnershipV1OpKind::Create,i));
    g.blocks[0].operations.push_back(op(OwnershipV1OpKind::ExitNormal));
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 1);
    const auto& order=r.normal_exits[0].cleanup_reverse_order;
    CHECK(order.size() == OwnershipDataflowV1::MaxOwners);
    for (std::size_t i=0; i<order.size(); ++i)
        CHECK(order[i] == order.size()-i-1);
    return true;
}
static bool accepts_max_operations_per_block() {
    auto g=graph(1);
    g.blocks[0].operations.push_back(op(OwnershipV1OpKind::Create));
    for (std::size_t i=0; i<OwnershipDataflowV1::MaxOperationsPerBlock-2; ++i)
        g.blocks[0].operations.push_back(op(OwnershipV1OpKind::Use));
    g.blocks[0].operations.push_back(op(OwnershipV1OpKind::ExitNormal));
    CHECK(g.blocks[0].operations.size() == OwnershipDataflowV1::MaxOperationsPerBlock);
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size() == 1);
    CHECK(r.normal_exits[0].cleanup_reverse_order ==
          std::vector<std::size_t>({0}));
    return true;
}
static bool accepts_max_blocks() {
    auto g=graph(1,OwnershipDataflowV1::MaxBlocks);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    for (std::size_t i=0; i+1<g.blocks.size(); ++i)
        g.blocks[i].successors={i+1};
    g.blocks.back().operations={op(OwnershipV1OpKind::ExitNormal)};
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.reachable_blocks == OwnershipDataflowV1::MaxBlocks);
    CHECK(r.analyzed_edges == OwnershipDataflowV1::MaxBlocks-1);
    CHECK(r.normal_exits.size() == 1);
    return true;
}
static bool accepts_max_edges() {
    auto g=graph(1,OwnershipDataflowV1::MaxBlocks);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks.back().operations={op(OwnershipV1OpKind::ExitNormal)};
    std::size_t edges=0;
    for (std::size_t i=0; i<g.blocks.size(); ++i) {
        for (std::size_t delta=1; delta<=4 && i+delta<g.blocks.size(); ++delta) {
            g.blocks[i].successors.push_back(i+delta);
            ++edges;
        }
        if (i<10) {
            g.blocks[i].successors.push_back(i+5);
            ++edges;
        }
    }
    CHECK(edges == OwnershipDataflowV1::MaxEdges);
    const auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.reachable_blocks == OwnershipDataflowV1::MaxBlocks);
    CHECK(r.analyzed_edges == OwnershipDataflowV1::MaxEdges);
    CHECK(r.normal_exits.size() == 1);
    return true;
}
static bool rejects_zero_owners() {
    auto g=graph(0);
    g.blocks[0].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("zero owners",std::move(g),
        "owner count outside bounded analysis limit");
}
static bool rejects_excess_owners() {
    auto g=graph(OwnershipDataflowV1::MaxOwners+1);
    g.blocks[0].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("too many owners",std::move(g),
        "owner count outside bounded analysis limit");
}
static bool rejects_excess_blocks() {
    auto g=graph(1,OwnershipDataflowV1::MaxBlocks+1);
    g.blocks[0].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("too many blocks",std::move(g),
        "invalid or unbounded CFG");
}
static bool rejects_excess_edges() {
    auto g=graph(1,2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks[0].successors.assign(OwnershipDataflowV1::MaxEdges+1,1);
    g.blocks[1].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("too many edges",std::move(g),
        "edge budget exceeded");
}
static bool rejects_excess_operations() {
    auto g=graph(1);
    g.blocks[0].operations.assign(OwnershipDataflowV1::MaxOperationsPerBlock+1,
        op(OwnershipV1OpKind::Use));
    return expect_reject("too many operations",std::move(g),
        "operation budget exceeded");
}
static bool rejects_invalid_successor() {
    auto g=graph(1);
    g.blocks[0].successors={1};
    return expect_reject("invalid successor",std::move(g),
        "invalid CFG successor");
}
static bool rejects_invalid_entry() {
    auto g=graph(1);
    g.entry=1;
    return expect_reject("invalid entry",std::move(g),
        "invalid or unbounded CFG");
}
static bool rejects_unknown_owner_reference() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create,0),
        op(OwnershipV1OpKind::Use,1),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("unknown owner index",std::move(g),
        "operation references unknown owner");
}
static bool rejects_exit_with_successors() {
    auto g=graph(1,2);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitNormal)};
    g.blocks[0].successors={1};
    g.blocks[1].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("exit with successors",std::move(g),
        "exit block must not have successors");
}
static bool rejects_exit_before_last_operation() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::ExitNormal),
        op(OwnershipV1OpKind::Use)};
    return expect_reject("exit not terminal",std::move(g),
        "exit operation must terminate its basic block");
}
static bool rejects_overlapping_mutable_borrows() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::BorrowMutable),
        op(OwnershipV1OpKind::BorrowMutable),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("overlapping mutable loans",std::move(g),
        "mutable borrow requires exclusive access");
}
static bool rejects_join_with_active_borrow_on_one_arm() {
    auto g=graph(1,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::BorrowShared)};
    g.blocks[1].successors={3};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("borrow state mismatch at join",std::move(g),
        "ownership states disagree at CFG join");
}
static bool rejects_take_to_same_owner() {
    auto g=graph(1);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create),
        op(OwnershipV1OpKind::Take,0,0),
        op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("self take",std::move(g),
        "take requires a distinct valid destination owner");
}


struct OwnershipCfgTestCase {
    const char* name;
    bool (*run)();
    bool negative;
};
struct OwnershipCfgTestTotals {
    std::size_t attempted=0;
    std::size_t positive=0;
    std::size_t negative=0;
    std::size_t failed=0;
};
static OwnershipCfgTestTotals run_all_cases(
    const OwnershipCfgTestCase* cases, std::size_t count) {
    OwnershipCfgTestTotals totals;
    for (std::size_t i=0; i<count; ++i) {
        const auto& testcase=cases[i];
        ++totals.attempted;
        if (testcase.negative) ++totals.negative;
        else ++totals.positive;
        bool passed=false;
        try {
            passed=testcase.run();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "OWNERSHIP_CFG_EXCEPTION|case=%s|what=%s\n",
                testcase.name,error.what());
        } catch (...) {
            std::fprintf(stderr, "OWNERSHIP_CFG_EXCEPTION|case=%s|what=unknown\n",
                testcase.name);
        }
        if (!passed) ++totals.failed;
        std::printf("OWNERSHIP_CFG|case=%s|kind=%s|result=%s\n",
            testcase.name,testcase.negative?"negative":"positive",
            passed?"PASS":"FAIL");
    }
    return totals;
}
static bool runner_canary_true() { return true; }
static bool runner_canary_false() { return false; }
int main(int argc, char** argv) {
    // A failing middle case must not suppress the final passing case.
    if (argc==2 && std::string(argv[1])=="--self-check-runner") {
        const OwnershipCfgTestCase canary[]={
            {"canary_before", &runner_canary_true, false},
            {"canary_expected_failure", &runner_canary_false, true},
            {"canary_after", &runner_canary_true, false}
        };
        const auto totals=run_all_cases(canary,sizeof(canary)/sizeof(canary[0]));
        const bool valid=totals.attempted==3 && totals.failed==1 &&
            totals.positive==2 && totals.negative==1;
        std::printf("OWNERSHIP_CFG_HARNESS|attempted=%zu|expected_failures=1|observed_failures=%zu|result=%s\n",
            totals.attempted,totals.failed,valid?"PASS":"FAIL");
        return valid?0:1;
    }
    if (argc!=1) {
        std::fprintf(stderr,"OWNERSHIP_CFG|error=unsupported_argument\n");
        return 2;
    }
    const OwnershipCfgTestCase cases[]={
        {"normal_cleanup_reverse_order", &normal_cleanup_reverse_order, false},
        {"take_transfers_to_destination", &take_transfers_to_destination, false},
        {"explicit_drop_not_repeated", &explicit_drop_not_repeated, false},
        {"shared_borrow_release", &shared_borrow_release, false},
        {"mutable_borrow_release", &mutable_borrow_release, false},
        {"mutable_borrow_allows_exclusive_write", &mutable_borrow_allows_exclusive_write, false},
        {"cleanup_uses_acquisition_not_declaration_order", &cleanup_uses_acquisition_not_declaration_order, false},
        {"acquisition_history_survives_move_and_drop", &acquisition_history_survives_move_and_drop, false},
        {"join_identical_ownership_states", &join_identical_ownership_states, false},
        {"borrowed_then_released_diamond_join", &borrowed_then_released_diamond_join, false},
        {"both_branches_drop_before_join", &both_branches_drop_before_join, false},
        {"take_acquisition_order_across_blocks", &take_acquisition_order_across_blocks, false},
        {"accepts_max_owners", &accepts_max_owners, false},
        {"accepts_max_operations_per_block", &accepts_max_operations_per_block, false},
        {"accepts_max_blocks", &accepts_max_blocks, false},
        {"accepts_max_edges", &accepts_max_edges, false},
        {"rejects_double_drop", &rejects_double_drop, true},
        {"rejects_use_after_take", &rejects_use_after_take, true},
        {"rejects_borrowed_drop", &rejects_borrowed_drop, true},
        {"rejects_mutable_alias", &rejects_mutable_alias, true},
        {"rejects_join_mismatch", &rejects_join_mismatch, true},
        {"rejects_unmodeled_exit", &rejects_unmodeled_exit, true},
        {"rejects_implicit_exit", &rejects_implicit_exit, true},
        {"rejects_duplicate_identity", &rejects_duplicate_identity, true},
        {"rejects_unbounded_cycle", &rejects_unbounded_cycle, true},
        {"rejects_take_without_destination", &rejects_take_without_destination, true},
        {"rejects_take_into_live_destination", &rejects_take_into_live_destination, true},
        {"rejects_use_before_create", &rejects_use_before_create, true},
        {"rejects_unknown_operation", &rejects_unknown_operation, true},
        {"rejects_unreachable_block", &rejects_unreachable_block, true},
        {"rejects_unbalanced_borrow", &rejects_unbalanced_borrow, true},
        {"rejects_mutation_during_shared_borrow", &rejects_mutation_during_shared_borrow, true},
        {"rejects_different_acquisition_order_at_join", &rejects_different_acquisition_order_at_join, true},
        {"rejects_end_borrow_without_loan", &rejects_end_borrow_without_loan, true},
        {"rejects_exit_with_owner_metadata", &rejects_exit_with_owner_metadata, true},
        {"rejects_stray_transfer_target", &rejects_stray_transfer_target, true},
        {"rejects_read_during_mutable_borrow", &rejects_read_during_mutable_borrow, true},
        {"rejects_exceptional_exit", &rejects_exceptional_exit, true},
        {"rejects_cancellation_exit", &rejects_cancellation_exit, true},
        {"rejects_take_with_outstanding_shared_borrow", &rejects_take_with_outstanding_shared_borrow, true},
        {"rejects_write_through_missing_mutable_loan", &rejects_write_through_missing_mutable_loan, true},
        {"rejects_end_missing_mutable_loan", &rejects_end_missing_mutable_loan, true},
        {"rejects_zero_owners", &rejects_zero_owners, true},
        {"rejects_excess_owners", &rejects_excess_owners, true},
        {"rejects_excess_blocks", &rejects_excess_blocks, true},
        {"rejects_excess_edges", &rejects_excess_edges, true},
        {"rejects_excess_operations", &rejects_excess_operations, true},
        {"rejects_invalid_successor", &rejects_invalid_successor, true},
        {"rejects_invalid_entry", &rejects_invalid_entry, true},
        {"rejects_unknown_owner_reference", &rejects_unknown_owner_reference, true},
        {"rejects_exit_with_successors", &rejects_exit_with_successors, true},
        {"rejects_exit_before_last_operation", &rejects_exit_before_last_operation, true},
        {"rejects_overlapping_mutable_borrows", &rejects_overlapping_mutable_borrows, true},
        {"rejects_join_with_active_borrow_on_one_arm", &rejects_join_with_active_borrow_on_one_arm, true},
        {"rejects_take_to_same_owner", &rejects_take_to_same_owner, true},
    };
    static_assert(sizeof(cases)/sizeof(cases[0])==55,
        "ownership CFG case registry count changed");
    const auto totals=run_all_cases(cases,sizeof(cases)/sizeof(cases[0]));
    const bool complete=totals.attempted==55 &&
        totals.positive==16 && totals.negative==39;
    std::printf("OWNERSHIP_DATAFLOW_V1|positive=%zu|negative=%zu|attempted=%zu|failed=%zu|result=%s\n",
        totals.positive,totals.negative,totals.attempted,
        totals.failed,(complete && totals.failed==0)?"PASS":"FAIL");
    return complete && totals.failed==0 ? 0 : 1;
}
