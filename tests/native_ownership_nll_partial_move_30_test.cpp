#include "oreslang/types/OwnershipLastUse.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::ownership;
using K = Operation::Kind;

namespace {
Operation op(K kind, const char* place = "x",
             std::optional<std::uint64_t> id = std::nullopt,
             std::optional<std::uint64_t> write = std::nullopt) {
    return {kind, place, id, write};
}
Graph single(std::vector<Operation> operations) {
    Graph g;
    g.entry = {{"x", PlaceState{kOwned}}};
    g.blocks = {{std::move(operations), {}}};
    return g;
}
Graph fields(std::vector<Operation> operations) {
    Graph g;
    g.entry = {{"p.a", PlaceState{kOwned}}, {"p.b", PlaceState{kOwned}}};
    g.blocks = {{std::move(operations), {}}};
    return g;
}
bool rejects(const std::function<void()>& attempt) {
    try { attempt(); }
    catch (const std::runtime_error& error) {
        return std::string(error.what()).find("ownership violation:") !=
            std::string::npos;
    }
    return false;
}
void needs(bool result, const char* label, unsigned& count) {
    ++count;
    if (!result)
        throw std::runtime_error(std::string("assertion ") +
            std::to_string(count) + ": " + label);
}
}

int main() {
  unsigned count = 0;
  try {
    // 1..10: exactly-one field reinitialization cannot resurrect an aggregate
    // when another field remains moved, or a join has MaybeMoved state.
    needs((verify(single({op(K::Move),op(K::Reinitialize,"x",std::nullopt,101),op(K::Read)})),true),
        "moved scalar explicitly reinitialized",count);
    needs(rejects([]{ verify(single({op(K::Reinitialize,"x",std::nullopt,102)})); }),
        "no reinit of live owner",count);
    needs(rejects([]{ verify(single({op(K::Move),op(K::Reinitialize,"x",std::nullopt,103),
                                    op(K::Reinitialize,"x",std::nullopt,104)})); }),
        "double reinitialization denied",count);
    needs(rejects([]{ verify(single({op(K::Move),op(K::Read)})); }),
        "moved owner remains unreadable",count);
    needs((verify(fields({op(K::Move,"p.a"),op(K::Reinitialize,"p.a",std::nullopt,105),
                          op(K::ReadObject,"p")})),true),
        "repaired field restores whole-object read",count);
    needs(rejects([]{ verify(fields({op(K::Move,"p.a"),op(K::ReadObject,"p")})); }),
        "partially moved object cannot be read",count);
    needs((verify(fields({op(K::Move,"p.a"),op(K::Read,"p.b")})),true),
        "independent field remains readable",count);
    needs(rejects([]{ verify(fields({op(K::Reinitialize,"p.b",std::nullopt,106)})); }),
        "cannot overwrite live sibling",count);
    needs(rejects([]{
        Graph g = single({});
        g.blocks = {{{},{1,2}},{{op(K::Move)},{3}},{{},{3}},
                    {{op(K::Reinitialize,"x",std::nullopt,107)},{}}};
        verify(g);
    }),"maybe-moved join cannot reinitialize",count);
    needs(rejects([]{ verify(single({
        op(K::BorrowShared,"x",11),op(K::Move),op(K::Reinitialize,"x",std::nullopt,108),
        op(K::ReleaseShared,"x",11)})); }),
        "active loan blocks field move/reinit",count);

    // 11..18: suspense boundaries reject live borrow; explicit release is
    // the only permissible boundary without a typed cross-suspend proof.
    needs((verify(single({op(K::Suspend)})),true),"suspend unborrowed",count);
    needs(rejects([]{ verify(single({op(K::BorrowShared,"x",11),
                                      op(K::Suspend),op(K::ReleaseShared,"x",11)})); }),
        "live shared loan cannot suspend",count);
    needs(rejects([]{ verify(single({op(K::BorrowExclusive,"x",12),
                                      op(K::Suspend),op(K::ReleaseExclusive,"x",12)})); }),
        "live exclusive loan cannot suspend",count);
    needs((verify(single({op(K::BorrowShared,"x",11),
                          op(K::ReleaseShared,"x",11),op(K::Suspend)})),true),
        "explicitly ended shared loan permits suspension",count);
    needs((verify(single({op(K::BorrowExclusive,"x",12),
                          op(K::ReleaseExclusive,"x",12),op(K::Suspend)})),true),
        "exclusive release before suspension",count);
    needs(rejects([]{ verify(single({op(K::UseSharedLoan,"x",13)})); }),
        "unissued shared token cannot be used",count);
    needs(rejects([]{ verify(single({op(K::BorrowShared,"x",11),
                                      op(K::UseSharedLoan,"x",12),
                                      op(K::ReleaseShared,"x",11)})); }),
        "foreign shared token rejected",count);
    needs(rejects([]{ verify(single({op(K::BorrowExclusive,"x",12),
                                      op(K::UseSharedLoan,"x",12),
                                      op(K::ReleaseExclusive,"x",12)})); }),
        "exclusive loan cannot be used as shared",count);

    // 19..30: the optional stage-zero NLL adapter is source/host-only:
    // explicit compiler-use evidence, no branch, loop or guest promotion.
    const std::vector<Operation> sharedOps = {
        op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
        op(K::UseSharedLoan,"x",11),op(K::Move)
    };
    const ProvenLastUse proof{11,"x",K::BorrowShared,0,2};
    needs((lower_explicit_last_use_evidence_v1(single(sharedOps),{proof})
        .blocks[0].operations.size() == 5),
        "last shared use inserts exactly one release",count);
    needs((lower_explicit_last_use_evidence_v1(single({
        op(K::BorrowExclusive,"x",12),op(K::UseExclusiveLoan,"x",12),
        op(K::Move)}),{{12,"x",K::BorrowExclusive,0,1}})
        .blocks[0].operations.size() == 4),
        "exclusive last use inserts release before owned move",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(
        single(sharedOps),{}); }), "missing complete borrow proof denied",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(
        single(sharedOps),{{12,"x",K::BorrowShared,0,2}}); }),
        "foreign last use token denied",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(
        single(sharedOps),{{11,"x",K::BorrowShared,0,1}}); }),
        "claimed last use before true final use denied",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(
        single(sharedOps),{proof,proof}); }),
        "duplicate proof rejected",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(
        single({op(K::BorrowShared,"x",11),
                op(K::UseSharedLoan,"x",11),
                op(K::ReleaseShared,"x",11)}),
        {{11,"x",K::BorrowShared,0,1}}); }),
        "explicit and inferred release cannot mix",count);
    needs(rejects([&]{
        Graph g = single({op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11)});
        g.blocks[0].successors={0};
        lower_explicit_last_use_evidence_v1(g,{{11,"x",K::BorrowShared,0,1}});
    }), "loop cannot claim straight-line NLL",count);
    needs(rejects([&]{
        Graph g = single({op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11)});
        g.blocks.push_back({{}, {}});
        lower_explicit_last_use_evidence_v1(g,{{11,"x",K::BorrowShared,0,1}});
    }), "branch/multi-block CFG cannot claim NLL",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(single({
        op(K::BorrowShared,"x",11),op(K::UseExclusiveLoan,"x",11)}),
        {{11,"x",K::BorrowShared,0,1}}); }),
        "wrong loan use mode denied",count);
    needs(rejects([&]{ lower_explicit_last_use_evidence_v1(single({
        op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
        op(K::UseSharedLoan,"x",11),op(K::Suspend)}),
        {{11,"x",K::BorrowShared,0,1}}); }),
        "hidden post-last-use before suspension denied",count);
    needs((lower_explicit_last_use_evidence_v1(single({
        op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
        op(K::Suspend),op(K::Move)}),
        {{11,"x",K::BorrowShared,0,1}}).blocks[0].operations.size() == 5),
        "proven release precedes suspension and owner move",count);
    if (count != 30) return 50;
    std::cout << "PASS 30/30 moved-field and explicit-loan-use CFG contracts "
                 "(HOST_CPP17_ONLY; NLL LOWERING NOT INTEGRATED)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL CFG proof case " << count << ": " << error.what() << '\n';
    return 1;
  }
}
