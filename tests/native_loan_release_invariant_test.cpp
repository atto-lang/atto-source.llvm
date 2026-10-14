// Private invariant test: compile the real ownership implementation into this
// translation unit to exercise loan accounting that is intentionally not API.
#include "oreslang/types/OwnershipChecker.cpp"

#include <iostream>
#include <stdexcept>
#include <string>

int main() {
    using namespace oreslang;
    Locals owners;
    owners.try_emplace("root");
    const Loan shared{"root", false};
    const Loan exclusive{"root", true};

    auto must_reject_inactive = [&](const Loan& loan) {
        try {
            release(loan, owners);
        } catch (const std::logic_error& error) {
            if (std::string(error.what()).find("releasing inactive") == std::string::npos)
                throw;
            return;
        }
        throw std::runtime_error("inactive native loan release incorrectly admitted");
    };

    // Releasing without acquisition must never wrap size_t or silently reset
    // a nonexistent exclusive borrow.
    must_reject_inactive(shared);
    must_reject_inactive(exclusive);
    if (owners.at("root").shared_loans != 0 || owners.at("root").exclusive_loan)
        throw std::runtime_error("rejected release mutated ownership state");

    owners.at("root").shared_loans = 2;
    release(shared, owners);
    release(shared, owners);
    must_reject_inactive(shared);
    if (owners.at("root").shared_loans != 0)
        throw std::runtime_error("balanced shared-loan count incorrect");

    owners.at("root").exclusive_loan = true;
    release(exclusive, owners);
    must_reject_inactive(exclusive);
    if (owners.at("root").exclusive_loan)
        throw std::runtime_error("balanced exclusive-loan state incorrect");

    std::cout << "PASS native loan accounting: zero-underflow, double release, balanced shared/exclusive\n";
}
