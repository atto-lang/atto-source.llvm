#include "oreslang/types/OwnershipCfg.hpp"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace oreslang::ownership;
using Kind = Operation::Kind;

// Restricted HOST CFG adversarial checks. These do not claim compiler-produced
// typed MIR, LLVM IR census, optimized SSA or guest execution.
namespace {
int checks = 0;
void require(bool accepted, const char* case_name) {
    ++checks;
    if (!accepted) throw std::runtime_error(std::string("CFG regression failed: ") + case_name);
}
template <typename F>
bool ownership_rejects(F&& operation) {
    try { operation(); } catch (const std::runtime_error& error) {
        return std::string(error.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}
Graph starting_owned() {
    Graph g;
    g.entry = {{"x", PlaceState{kOwned}}, {"y", PlaceState{kOwned}}};
    return g;
}
}

int main() {
    try {
        // One lexical borrow occurs BEFORE the diamond. The two edges carry
        // the *same* loan identity and the join can release it exactly once.
        Graph common = starting_owned();
        common.blocks = {
            {{{Kind::BorrowShared, "x", 701}}, {1, 2}},
            {{{Kind::Read, "x"}}, {3}},
            {{{Kind::Read, "y"}}, {3}},
            {{{Kind::ReleaseShared, "x", 701}, {Kind::Move, "x"}}, {}}
        };
        require(!ownership_rejects([&] { verify(common); }), "common pre-branch loan succeeds");

        // A different token cannot release the pre-branch loan.
        Graph wrong_release = common;
        wrong_release.blocks[3].operations[0].loan_id = 702;
        require(ownership_rejects([&] { verify(wrong_release); }), "foreign release at phi");

        // Moving while a shared loan is active is prohibited even in a branch.
        Graph move_under_loan = common;
        move_under_loan.blocks[1].operations = {{Kind::Move, "x"}};
        require(ownership_rejects([&] { verify(move_under_loan); }), "move under shared loan");

        // Two independently minted loans, even on exclusive paths, are not
        // the single shared loan expected at a phi join.
        Graph different_sites = starting_owned();
        different_sites.blocks = {
            {{}, {1, 2}},
            {{{Kind::BorrowShared, "x", 711}}, {3}},
            {{{Kind::BorrowShared, "x", 712}}, {3}},
            {{{Kind::ReleaseShared, "x"}}, {}}
        };
        require(ownership_rejects([&] { verify(different_sites); }), "foreign loan histories at phi");

        // Explicitly reusing the same ID at two lexical sites is forgery.
        Graph reused_id = different_sites;
        reused_id.blocks[2].operations[0].loan_id = 711;
        require(ownership_rejects([&] { verify(reused_id); }), "duplicate loan ID across branches");

        // A move on only one predecessor poisons a joined read.
        Graph partial_move = starting_owned();
        partial_move.blocks = {
            {{}, {1, 2}},
            {{{Kind::Move, "x"}}, {3}},
            {{}, {3}},
            {{{Kind::Read, "x"}}, {}}
        };
        require(ownership_rejects([&] { verify(partial_move); }), "moved predecessor at join");

        // A disjoint tracked field/variable may remain readable when another
        // owner is moved on one branch.
        partial_move.blocks[3].operations = {{Kind::Read, "y"}};
        require(!ownership_rejects([&] { verify(partial_move); }), "independent value at join");

        // A loop backedge moving the read target is unsafe on the next trip.
        Graph loop = starting_owned();
        loop.blocks = {
            {{}, {1}},
            {{{Kind::Read, "x"}}, {2, 3}},
            {{}, {1}},
            {{}, {}}
        };
        require(!ownership_rejects([&] { verify(loop); }), "loop without ownership effects");
        loop.blocks[2].operations = {{Kind::Move, "x"}};
        require(ownership_rejects([&] { verify(loop); }), "moved value on loop backedge");

        // Even unreachable blocks must not hide out-of-range CFG successors.
        Graph invalid_edge = starting_owned();
        invalid_edge.blocks = {
            {{}, {}},
            {{}, {2}}
        };
        require(ownership_rejects([&] { verify(invalid_edge); }), "invalid edge in dead block");

        // Borrowing an overlapping whole-object place and projection must
        // fail before any read or release can bypass the alias.
        Graph ambiguous;
        ambiguous.entry = {{"record", PlaceState{kOwned}},
                           {"record.field", PlaceState{kOwned}}};
        ambiguous.blocks = {{{}, {}}};
        require(ownership_rejects([&] { verify(ambiguous); }), "whole versus field alias");

        // A site cannot mint the reserved zero/entry-loan token.
        Graph reserved = starting_owned();
        reserved.blocks = {{{{Kind::BorrowExclusive, "x", 0}}, {}}};
        require(ownership_rejects([&] { verify(reserved); }), "reserved entry identity");

        require(checks == 12, "must execute all 12 focused CFG cases");
        std::cout << "HOST_CFG_ONLY 12/12; compiler/SSA/guest proof NOT_EXECUTED\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << failure.what() << '\n';
        return 1;
    }
}
