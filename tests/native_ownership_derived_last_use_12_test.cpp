#include "oreslang/types/OwnershipLastUse.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace oreslang::ownership;
using K = Operation::Kind;
Operation op(K k, const char* p = "x",
             std::optional<std::uint64_t> token = std::nullopt) {
    return {k,p,token};
}
Graph graph(std::vector<Operation> operations) {
    Graph g;
    g.entry = {{"x", PlaceState{kOwned}},{"y",PlaceState{kOwned}}};
    g.blocks = {{std::move(operations),{}}};
    return g;
}
bool rejects(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}
void check(bool ok, const char* label, unsigned& count) {
    ++count;
    if (!ok) throw std::runtime_error(std::string("assertion ") +
                std::to_string(count) + ": " + label);
}
} // namespace

int main() {
    unsigned count = 0;
    try {
        check(lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
            op(K::Move)})).blocks[0].operations.size()==4,
            "derived shared release before move",count);
        check(lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
            op(K::UseSharedLoan,"x",11),op(K::Move)}))
            .blocks[0].operations.size()==5,
            "last of two actual use sites",count);
        check(lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
            op(K::BorrowShared,"y",12),op(K::UseSharedLoan,"y",12),
            op(K::Move,"x"),op(K::Move,"y")}))
            .blocks[0].operations.size()==8,
            "derive two independent lexical loan releases",count);
        check(lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowExclusive,"x",13),op(K::UseExclusiveLoan,"x",13),
            op(K::Move)})).blocks[0].operations.size()==4,
            "exclusive token stays exclusive",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::UseSharedLoan,"x",11)})); }),
            "unknown loan use token rejected",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseExclusiveLoan,"x",11)})); }),
            "loan mode may not be forged",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"y",11)})); }),
            "loan cannot migrate between source places",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::BorrowShared,"y",11),
            op(K::UseSharedLoan,"x",11)})); }),
            "loan ID cannot be duplicated across roots",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11)})); }),
            "no final use = incomplete stream",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
            op(K::ReleaseShared,"x",11)})); }),
            "manual and derived releases may not mix",count);
        check(rejects([]{
            auto g=graph({op(K::BorrowShared,"x",11),
                          op(K::UseSharedLoan,"x",11)});
            g.blocks[0].successors={0};
            lower_closed_world_loan_tokens_v1(g);
        }),"loop remains unproven",count);
        check(rejects([]{ lower_closed_world_loan_tokens_v1(graph({
            op(K::BorrowShared,"x",11),op(K::UseSharedLoan,"x",11),
            op(K::Read,"x")})); }),
            "unclassified post-borrow read rejected",count);
        if (count != 12) return 13;
        std::cout << "PASS 12/12 derived closed-world loan-use cases "
                     "(HOST_CPP17_ONLY; NOT GUEST SSA)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "failed case " << count << ": " << e.what() << '\n';
        return 1;
    }
}
