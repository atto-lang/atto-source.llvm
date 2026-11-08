#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include <cstdio>
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

int main() {
    const bool ok =
      normal_cleanup_reverse_order() && take_transfers_to_destination() &&
      explicit_drop_not_repeated() && shared_borrow_release() &&
      mutable_borrow_release() && mutable_borrow_allows_exclusive_write() &&
      rejects_double_drop() &&
      rejects_use_after_take() && rejects_borrowed_drop() &&
      rejects_mutable_alias() && rejects_join_mismatch() &&
      rejects_unmodeled_exit() && rejects_implicit_exit() &&
      rejects_duplicate_identity() && rejects_unbounded_cycle() &&
      rejects_take_without_destination() && rejects_take_into_live_destination() &&
      rejects_use_before_create() && rejects_unknown_operation() &&
      rejects_unreachable_block() && rejects_unbalanced_borrow() &&
      rejects_mutation_during_shared_borrow() &&
      cleanup_uses_acquisition_not_declaration_order() &&
      rejects_different_acquisition_order_at_join() &&
      rejects_end_borrow_without_loan() && rejects_exit_with_owner_metadata() &&
      rejects_stray_transfer_target() &&
      rejects_read_during_mutable_borrow() &&
      rejects_exceptional_exit() && rejects_cancellation_exit() &&
      rejects_take_with_outstanding_shared_borrow() &&
      rejects_write_through_missing_mutable_loan() &&
      rejects_end_missing_mutable_loan();
    if (!ok) return 1;
    std::puts("OWNERSHIP_DATAFLOW_V1|positive=7|negative=26|result=PASS");
    return 0;
}
