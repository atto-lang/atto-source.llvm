#include "oreslang/types/OwnershipCfg.hpp"

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

Operation op(K kind, const char* key = "x",
             std::optional<std::uint64_t> loan = std::nullopt,
             std::optional<std::uint64_t> write = std::nullopt) {
    return {kind, key, loan, write};
}
Graph graph(std::vector<Operation> operations) {
    Graph g;
    g.entry = {{"x", PlaceState{kOwned}}};
    g.blocks = {{std::move(operations), {}}};
    return g;
}
bool denied(const std::function<void()>& action) {
    try { action(); }
    catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}
void check(bool condition, const char* label, unsigned& count) {
    ++count;
    if (!condition)
        throw std::runtime_error(std::string("failed invariant ") +
                                 std::to_string(count) + ": " + label);
}
} // namespace

int main() {
    unsigned count = 0;
    try {
        check(denied([] { verify(graph({op(K::Read, "x", 51)})); }),
              "opaque read cannot impersonate a loan use", count);
        check(denied([] { verify(graph({op(K::ReadObject, "p", 51)})); }),
              "whole-object read cannot impersonate a loan use", count);
        check(denied([] { verify(graph({op(K::Move, "x", 51)})); }),
              "move cannot carry a loan use token", count);
        check(denied([] { verify(graph({op(K::DropIfOwned, "x", 51)})); }),
              "conditional drop cannot carry a loan use token", count);
        check(denied([] { verify(graph({op(K::Suspend, "x", 51)})); }),
              "suspension cannot carry forged loan provenance", count);
        check(denied([] {
            State s{{"x", PlaceState{kOwned}}};
            apply(op(K::Read, "x", 51), s);
        }), "direct apply rejects forged read token", count);
        check(denied([] {
            State s{{"x", PlaceState{kOwned}}};
            apply(op(K::Suspend, "x", 51), s);
        }), "direct apply rejects forged suspend token", count);
        check(denied([] {
            State s{{"x", PlaceState{kOwned}}};
            apply(op(K::Read, "x", std::nullopt, 77), s);
        }), "direct apply rejects forged write metadata", count);
        check(denied([] {
            auto g = graph({});
            g.blocks.push_back({{op(K::Read, "x", 51)}, {}});
            verify(g);
        }), "unreachable block cannot hide forged token metadata", count);
        check((verify(graph({op(K::BorrowShared, "x", 51),
                             op(K::UseSharedLoan, "x", 51),
                             op(K::ReleaseShared, "x", 51),
                             op(K::Move)})), true),
              "legitimate typed loan issue-use-release still verifies", count);
        if (count != 10) throw std::runtime_error("wrong test count");
        std::cout << "PASS 10/10 native loan metadata admission checks "
                     "(HOST_CPP17_ONLY; NOT GUEST SSA)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "failed invariant " << count << ": " << e.what() << '\n';
        return 1;
    }
}
