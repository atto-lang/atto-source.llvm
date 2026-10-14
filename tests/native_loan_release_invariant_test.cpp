// Regression exercising the real (private) native ownership implementation.
// Tickets must make double release and forged release detectable even while
// OTHER shared/exclusive compiler-side loans remain live.
#include "oreslang/types/OwnershipChecker.cpp"

#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

int main() {
    using namespace oreslang;
    Locals owners;
    owners.try_emplace("root");
    owners.at("root").mutable_owner = true;

    auto borrow = [](bool exclusive) {
        Expression expression;
        expression.kind = exclusive ? Expression::Kind::BorrowMut : Expression::Kind::Borrow;
        expression.left = std::make_unique<Expression>();
        expression.left->kind = Expression::Kind::Variable;
        expression.left->name = "root";
        return expression;
    };
    auto shared_expr = borrow(false);
    auto mutable_expr = borrow(true);

    auto expect_inactive = [&](const Loan& loan) {
        try {
            release(loan, owners);
        } catch (const std::logic_error& error) {
            if (std::string(error.what()).find("releasing inactive") == std::string::npos)
                throw;
            return;
        }
        throw std::runtime_error("invalid native loan release unexpectedly admitted");
    };

    expect_inactive(Loan{"root", false, 0});
    expect_inactive(Loan{"root", true, 0});

    const Loan first = acquire(shared_expr, Mode::Borrow, owners);
    const Loan second = acquire(shared_expr, Mode::Borrow, owners);
    if (!first.ticket || !second.ticket || first.ticket == second.ticket ||
        owners.at("root").shared_loans != 2)
        throw std::runtime_error("two shared borrows need distinct live loan identities");

    // A stale/forged release must not consume the unrelated second loan.
    release(first, owners);
    expect_inactive(first);
    expect_inactive(Loan{"root", false, second.ticket + 99});
    expect_inactive(Loan{"root", true, second.ticket});
    if (owners.at("root").shared_loans != 1 ||
        owners.at("root").live_shared_tickets.count(second.ticket) != 1)
        throw std::runtime_error("replay or forged loan release damaged a separate live loan");
    release(second, owners);
    expect_inactive(second);

    const Loan exclusive = acquire(mutable_expr, Mode::BorrowMut, owners);
    if (!exclusive.ticket || !owners.at("root").exclusive_loan)
        throw std::runtime_error("exclusive loan not acquired");
    expect_inactive(Loan{"root", true, exclusive.ticket + 1});
    expect_inactive(Loan{"root", false, exclusive.ticket});
    if (!owners.at("root").exclusive_loan)
        throw std::runtime_error("forged loan release disabled active exclusive loan");
    release(exclusive, owners);
    expect_inactive(exclusive);

    const Loan newer = acquire(shared_expr, Mode::Borrow, owners);
    if (newer.ticket == first.ticket || newer.ticket == second.ticket)
        throw std::runtime_error("retired native loan identity was reused");
    expect_inactive(first);
    release(newer, owners);

    auto& owner = owners.at("root");
    if (owner.shared_loans != 0 || owner.exclusive_loan ||
        !owner.live_shared_tickets.empty() || owner.live_exclusive_ticket != 0)
        throw std::runtime_error("all native loans must retire");

    owner.next_loan_ticket = std::numeric_limits<std::size_t>::max();
    try {
        (void)acquire(shared_expr, Mode::Borrow, owners);
        throw std::runtime_error("loan ticket overflow was admitted");
    } catch (const std::runtime_error& error) {
        if (std::string(error.what()).find("loan identity space exhausted") == std::string::npos)
            throw;
    }
    if (owner.shared_loans != 0 || !owner.live_shared_tickets.empty())
        throw std::runtime_error("rejected loan ticket overflow mutated state");
    std::cout << "PASS native loan tickets: replay, forgery, wrong mode, double release, retirement, overflow\n";
}
