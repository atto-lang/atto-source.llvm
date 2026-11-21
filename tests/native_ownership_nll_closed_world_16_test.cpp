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
using Kind = Operation::Kind;
Operation op(Kind kind, const char* place = "x",
             std::optional<std::uint64_t> loan = std::nullopt,
             std::optional<std::uint64_t> write = std::nullopt) {
    return {kind,place,loan,write};
}
Graph graph(std::vector<Operation> operations) {
    Graph result;
    result.entry = {{"x", PlaceState{kOwned}}};
    result.blocks = {{std::move(operations),{}}};
    return result;
}
bool denied(const std::function<void()>& f) {
    try { f(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}
void expect(bool yes, const char* label, unsigned& count) {
    ++count;
    if (!yes) throw std::runtime_error(std::string("failed assertion ") +
                                       std::to_string(count) + ": " + label);
}
} // namespace

int main() {
    unsigned count = 0;
    try {
        // 1..8: a state transition cannot fabricate a source-level write.
        expect(denied([]{ verify(graph({op(Kind::Move),
            op(Kind::Reinitialize)})); }), "bare reinit denied", count);
        expect(denied([]{ verify(graph({op(Kind::Move),
            op(Kind::Reinitialize,"x",std::nullopt,0)})); }),
            "zero write ticket denied", count);
        expect(denied([]{ verify(graph({op(Kind::Move),
            op(Kind::Reinitialize,"x",11)})); }),
            "loan token cannot stand in for a write token", count);
        expect(denied([]{ verify(graph({op(Kind::Move),
            op(Kind::Reinitialize,"x",11,12)})); }),
            "write operation cannot carry a loan identity", count);
        expect(denied([]{ verify(graph({op(Kind::Read,"x",std::nullopt,71)})); }),
            "read may not impersonate a write-site", count);
        expect(denied([]{ verify(graph({op(Kind::Move),
            op(Kind::Reinitialize,"x",std::nullopt,21),
            op(Kind::Move),op(Kind::Reinitialize,"x",std::nullopt,21)})); }),
            "a ticket may not be reused for two writes", count);
        expect((verify(graph({op(Kind::Move),
            op(Kind::Reinitialize,"x",std::nullopt,22),op(Kind::Read)})),true),
            "one moved value can reinitialize on a distinct write-site", count);
        expect(denied([]{ verify(graph({op(Kind::Reinitialize,"x",std::nullopt,23)})); }),
            "a live owner cannot be overwritten by restoration", count);

        // 9..13: opaque owner accesses cannot hide borrowed uses from the
        // stage-zero last-use adapter (though the general CFG may allow reads).
        const std::vector<Operation> source = {
            op(Kind::BorrowShared,"x",31),
            op(Kind::UseSharedLoan,"x",31),op(Kind::Move)
        };
        const ProvenLastUse proof{31,"x",Kind::BorrowShared,0,1};
        expect(lower_explicit_last_use_evidence_v1(graph(source),{proof})
                   .blocks[0].operations.size()==4,
               "annotated loan use then ownership move remains a candidate",count);
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::UseSharedLoan,"x",31),
            op(Kind::Read)}),{proof}); }),
            "opaque read after alleged final use denied",count);
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::Read),
            op(Kind::UseSharedLoan,"x",31)}),
            {{31,"x",Kind::BorrowShared,0,2}}); }),
            "opaque read before declared final use denied",count);
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::Read),op(Kind::BorrowShared,"x",31),
            op(Kind::UseSharedLoan,"x",31),op(Kind::ReadObject,"p")}),
            {{31,"x",Kind::BorrowShared,1,2}}); }),
            "unclassified whole-object read denied even on other name",count);
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::UseSharedLoan,"x",31),
            op(Kind::Read,"other")}),{proof}); }),
            "independent read remains conservatively unproven",count);

        // 14..16: last-use proof is still an advisory input, not an SSA
        // completeness certificate. No guest lowering may call it yet.
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::UseSharedLoan,"x",31),
            op(Kind::UseSharedLoan,"x",31)}),{proof}); }),
            "explicit borrowed use after alleged last use denied",count);
        expect(denied([&]{ lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::UseSharedLoan,"x",31),
            op(Kind::Suspend),op(Kind::Read)}),{proof}); }),
            "suspension followed by opaque read denied",count);
        expect(lower_explicit_last_use_evidence_v1(graph({
            op(Kind::BorrowShared,"x",31),op(Kind::UseSharedLoan,"x",31),
            op(Kind::Suspend),op(Kind::Move)}),{proof})
            .blocks[0].operations.size()==5,
            "annotated release before suspension and owner move",count);

        if (count != 16) throw std::runtime_error("wrong test count");
        std::cout << "PASS 16/16 write-ticket and closed-world loan-use checks "
                     "(HOST_CPP17_ONLY; NOT SSA-PROVEN)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAILED invariant " << count << ": " << e.what() << '\n';
        return 1;
    }
}
