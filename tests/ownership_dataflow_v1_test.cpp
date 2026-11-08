#include "oreslang/backend/OwnershipDataflowV1.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); return false; } } while (0)

static OwnershipV1Op op(OwnershipV1OpKind kind, std::size_t owner = 0) {
    return {kind, owner};
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
static bool take_disarms_cleanup() {
    auto g = graph(2);
    g.blocks[0].operations = {op(OwnershipV1OpKind::Create,0),op(OwnershipV1OpKind::Create,1),op(OwnershipV1OpKind::Take,1),op(OwnershipV1OpKind::ExitNormal)};
    auto r=OwnershipDataflowV1::analyze(g);
    CHECK(r.normal_exits.size()==1);
    CHECK(r.normal_exits[0].cleanup_reverse_order.size()==1);
    CHECK(r.normal_exits[0].cleanup_reverse_order[0]==0);
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
static bool expect_reject(const char* name, OwnershipV1Graph g) {
    try { (void)OwnershipDataflowV1::analyze(g); }
    catch (const std::runtime_error&) { return true; }
    std::fprintf(stderr,"FAIL expected rejection: %s\n",name); return false;
}
static bool rejects_double_drop() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("double drop",std::move(g));
}
static bool rejects_use_after_take() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::Take),op(OwnershipV1OpKind::Use),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("use after take",std::move(g));
}
static bool rejects_borrowed_drop() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::Drop),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("drop with borrow",std::move(g));
}
static bool rejects_mutable_alias() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::BorrowShared),op(OwnershipV1OpKind::BorrowMutable),op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("shared + mut borrow",std::move(g));
}
static bool rejects_join_mismatch() {
    auto g=graph(1,4);
    g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    g.blocks[0].successors={1,2};
    g.blocks[1].operations={op(OwnershipV1OpKind::Take)};
    g.blocks[1].successors={3};
    g.blocks[2].successors={3};
    g.blocks[3].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("join state mismatch",std::move(g));
}
static bool rejects_unmodeled_exit() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create),op(OwnershipV1OpKind::ExitSuspend)};
    return expect_reject("suspension exit",std::move(g));
}
static bool rejects_implicit_exit() {
    auto g=graph(1);g.blocks[0].operations={op(OwnershipV1OpKind::Create)};
    return expect_reject("missing explicit exit",std::move(g));
}
static bool rejects_duplicate_identity() {
    auto g=graph(2);g.owners[1]=g.owners[0];
    g.blocks[0].operations={op(OwnershipV1OpKind::ExitNormal)};
    return expect_reject("duplicate owner",std::move(g));
}
int main() {
    const bool ok =
      normal_cleanup_reverse_order() && take_disarms_cleanup() &&
      explicit_drop_not_repeated() && shared_borrow_release() &&
      mutable_borrow_release() && rejects_double_drop() &&
      rejects_use_after_take() && rejects_borrowed_drop() &&
      rejects_mutable_alias() && rejects_join_mismatch() &&
      rejects_unmodeled_exit() && rejects_implicit_exit() &&
      rejects_duplicate_identity();
    if (!ok) return 1;
    std::puts("OWNERSHIP_DATAFLOW_V1|positive=5|negative=8|result=PASS");
    return 0;
}
