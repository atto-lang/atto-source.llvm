#include "oreslang/types/OwnershipMirEvidence.hpp"

#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::ownership;
using K = Operation::Kind;
using UC = MirUseClass;
using SO = FieldStoreOpcode;

Operation op(K kind, const char* place = "x",
             std::optional<std::uint64_t> loan = std::nullopt,
             std::optional<std::uint64_t> write = std::nullopt) {
    return {kind, place, loan, write};
}
Graph graph(std::vector<Operation> ops) {
    Graph g;
    g.entry = {{"x", PlaceState{kOwned}}, {"y", PlaceState{kOwned}}};
    g.blocks = {{std::move(ops), {}}};
    return g;
}
Graph loan_graph() {
    return graph({op(K::BorrowShared, "x", 31),
                  op(K::UseSharedLoan, "x", 31),
                  op(K::ReleaseShared, "x", 31)});
}
Graph write_graph() {
    return graph({op(K::Move), op(K::Reinitialize, "x", std::nullopt, 71),
                  op(K::Read)});
}
TypedMirLoanUse u(UC kind = UC::Load) { return {41, 1, 31, "x", kind, 51}; }
TypedMirFieldWrite w() { return {1, 71, "x", 61}; }
LoweredFieldStore s() { return {71, 81, "x", 61, SO::NativeStore}; }
MirReceiptCandidate inspect(const Graph& g,
        const std::vector<TypedMirLoanUse>& ssa = {},
        const std::vector<TypedMirLoanUse>& lowered = {},
        const std::vector<TypedMirFieldWrite>& writes = {},
        const std::vector<LoweredFieldStore>& stores = {}) {
    return inspect_typed_mir_receipt_candidate_v1(g, ssa, lowered, writes, stores);
}
bool denied(const std::function<void()>& f) {
    try { f(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}
void check(bool ok, const char* label, unsigned& n) {
    ++n;
    if (!ok) throw std::runtime_error(std::string("case ") + std::to_string(n)
        + ": " + label);
}
int main() {
    unsigned n = 0;
    try {
        const auto loan = loan_graph();
        const auto write = write_graph();
        check(!inspect(graph({op(K::Read)})).verified,
              "no claim of verification", n);
        const auto good=inspect(loan,{u()},{u()});
        check(!good.verified && !good.compiler_backed &&
              good.classified_uses == 1, "candidate remains advisory", n);
        check(inspect(loan,{u(UC::CallArgument)},{u(UC::CallArgument)})
                  .classified_uses == 1,"call argument use",n);
        check(inspect(loan,{u(UC::ClosureCapture)},{u(UC::ClosureCapture)})
                  .classified_uses == 1,"captured use",n);
        check(inspect(loan,{u(UC::PhiEdge)},{u(UC::PhiEdge)})
                  .classified_uses == 1,"phi use",n);
        check(inspect(write,{}, {},{w()},{s()}).matched_field_stores == 1,
              "store candidate nonpromoting",n);
        check(denied([&] {inspect(loan);}),"missing SSA use",n);
        check(denied([&] {inspect(loan,{u()});}),"missing lowered edge",n);
        check(denied([&] {inspect(loan,{u()},{u(),u()});}),"duplicate edge",n);
        check(denied([&] {inspect(loan,{u(),u()},{u()});}),"duplicate SSA use",n);
        check(denied([&] {
            auto wrong=u();wrong.loan_id=999;inspect(loan,{wrong},{wrong});
        }),"wrong loan id",n);
        check(denied([&] {
            auto wrong=u();wrong.place="y";inspect(loan,{wrong},{wrong});
        }),"wrong place",n);
        check(denied([&] {
            auto wrong=u();wrong.cfg_operation=0;inspect(loan,{wrong},{wrong});
        }),"wrong CFG site",n);
        check(denied([&] {
            auto wrong=u();wrong.site_id=0;inspect(loan,{wrong},{wrong});
        }),"zero use site",n);
        check(denied([&] {
            auto wrong=u();wrong.ssa_value_id=0;inspect(loan,{wrong},{wrong});
        }),"zero SSA value",n);
        check(denied([&] {
            inspect(loan,{u(UC::ExceptionUnwind)},{u(UC::ExceptionUnwind)});
        }),"exception unwind blocked",n);
        check(denied([&] {
            inspect(loan,{u(UC::Cancellation)},{u(UC::Cancellation)});
        }),"cancellation blocked",n);
        check(denied([&] {
            inspect(loan,{u(UC::Suspension)},{u(UC::Suspension)});
        }),"suspension blocked",n);
        check(denied([&] {
            auto wrong=u(static_cast<UC>(250));inspect(loan,{wrong},{wrong});
        }),"invalid use classification",n);
        check(denied([&] {inspect(write);}),"no MIR write",n);
        check(denied([&] {inspect(write,{}, {},{w()});}),"no store",n);
        check(denied([&] {
            auto bad=w();bad.ssa_value_id=0;inspect(write,{}, {},{bad},{s()});
        }),"no write value",n);
        check(denied([&] {
            auto bad=s();bad.instruction_id=0;
            inspect(write,{}, {},{w()},{bad});
        }),"no instruction id",n);
        check(denied([&] {
            auto bad=s();bad.opcode=SO::PhiOnly;
            inspect(write,{}, {},{w()},{bad});
        }),"phi is not a native store",n);
        check(denied([&] {
            auto bad=s();bad.place="y";inspect(write,{}, {},{w()},{bad});
        }),"wrong store place",n);
        check(denied([&] {
            auto bad=s();bad.ssa_value_id=60;
            inspect(write,{}, {},{w()},{bad});
        }),"wrong stored value",n);
        check(denied([&] {
            auto bad=w();bad.write_id=72;
            inspect(write,{}, {},{bad},{s()});
        }),"wrong write ticket",n);
        check(denied([&] {inspect(write,{}, {},{w(),w()},{s()});}),
              "duplicate MIR write",n);
        check(denied([&] {inspect(write,{}, {},{w()},{s(),s()});}),
              "duplicate store claim",n);
        check(denied([&] {
            auto g=loan;g.blocks[0].successors={0};inspect(g,{u()},{u()});
        }),"loop not admitted",n);
        if(n != 30) throw std::runtime_error("wrong test count");
        std::cout << "PASS 30/30 typed-MIR use/store candidate cases "
                     "(HOST_CPP17_ONLY; NOT SSA-COMPLETE OR GUEST-VERIFIED)\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "FAILED at " << n << ": " << e.what() << '\n';
        return 1;
    }
}
