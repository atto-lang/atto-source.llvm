#include "oreslang/types/OwnershipCfg.hpp"

// These ownership proofs must execute even in Release CMake builds.
// With -DNDEBUG every assert vanishes, creating a false-green safety gate
// and an unused-function -Werror build failure.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <functional>
#include <stdexcept>
#include <string>

using namespace oreslang::ownership;
using Kind = Operation::Kind;

static Operation op(Kind kind, const char* place) { return {kind, place}; }
static PlaceState cell(ValueMask state) { return {state, 0, false}; }

static bool rejected(const std::function<void()>& fn) {
    try { fn(); } catch (const std::runtime_error& e) {
        return std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    return false;
}

int main() {
    // Both incoming paths initialize the owner before the join.
    Graph both;
    both.entry = {{"x", cell(kUninitialized)}};
    both.blocks = {
        {{}, {1, 2}},
        {{{Kind::Initialize, "x"}}, {3}},
        {{{Kind::Initialize, "x"}}, {3}},
        {{{Kind::Read, "x"}}, {}}
    };
    auto result = verify(both);
    assert(result.incoming[3] && result.incoming[3]->at("x").possible == kOwned);

    // Initialization only on one predecessor is insufficient.
    Graph partial = both;
    partial.blocks[2].operations.clear();
    assert(rejected([&] { verify(partial); }));

    // Explicit class field moves invalidate a complete-object move/read.
    Graph fields;
    fields.entry = {{"p.a", cell(kOwned)}, {"p.b", cell(kOwned)}};
    fields.blocks = {
        {{}, {1, 2}},
        {{{Kind::Move, "p.a"}}, {3}},
        {{}, {3}},
        {{{Kind::ReadObject, "p"}}, {}}
    };
    assert(rejected([&] { verify(fields); }));
    fields.blocks[3].operations = {op(Kind::Read, "p.b")};
    verify(fields); // Disjoint still-owned field remains readable.

    // A definite whole-object transfer marks every field moved.
    Graph whole;
    whole.entry = fields.entry;
    whole.blocks = {{{op(Kind::MoveObject, "p"), op(Kind::Read, "p.b")}, {}}};
    assert(rejected([&] { verify(whole); }));

    // Whole-object reads must not bypass exclusive field borrow checks.
    Graph object_loan;
    object_loan.entry = {{"p.a", cell(kOwned)}, {"p.b", cell(kOwned)}};
    object_loan.blocks = {{{op(Kind::BorrowExclusive, "p.a"),
                            op(Kind::ReadObject, "p"),
                            op(Kind::ReleaseExclusive, "p.a")}, {}}};
    assert(rejected([&] { verify(object_loan); }));
    // Shared field loans may coexist with immutable whole-object reads.
    object_loan.blocks = {{{op(Kind::BorrowShared, "p.a"),
                            op(Kind::ReadObject, "p"),
                            op(Kind::ReleaseShared, "p.a")}, {}}};
    verify(object_loan);
    // Once the exclusive field loan is released, the object is readable.
    object_loan.blocks = {{{op(Kind::BorrowExclusive, "p.b"),
                            op(Kind::ReleaseExclusive, "p.b"),
                            op(Kind::ReadObject, "p")}, {}}};
    verify(object_loan);
    // A shared field loan still forbids moving the whole object.
    object_loan.blocks = {{{op(Kind::BorrowShared, "p.a"),
                            op(Kind::MoveObject, "p")}, {}}};
    assert(rejected([&] { verify(object_loan); }));

    Graph valid_loop;
    valid_loop.entry = {{"x", cell(kOwned)}};
    valid_loop.blocks = {
        {{}, {1}},
        {{{Kind::Read, "x"}}, {2, 3}},
        {{}, {1}},
        {{}, {}}
    };
    verify(valid_loop);

    // A loop backedge moving x poisons the next iteration's read.
    Graph invalid_loop = valid_loop;
    invalid_loop.blocks[2].operations = {op(Kind::Move, "x")};
    assert(rejected([&] { verify(invalid_loop); }));

    Graph borrow;
    borrow.entry = {{"x", cell(kOwned)}};
    borrow.blocks = {{{op(Kind::BorrowShared, "x"),
                       op(Kind::BorrowExclusive, "x")}, {}}};
    assert(rejected([&] { verify(borrow); }));
    borrow.blocks = {{{op(Kind::BorrowShared, "x"),
                       op(Kind::ReleaseShared, "x"),
                       op(Kind::BorrowExclusive, "x"),
                       op(Kind::ReleaseExclusive, "x"),
                       op(Kind::Move, "x")}, {}}};
    verify(borrow);

    Graph loan_join;
    loan_join.entry = borrow.entry;
    loan_join.blocks = {
        {{}, {1, 2}},
        {{{Kind::BorrowShared, "x"}}, {3}},
        {{}, {3}},
        {{}, {}}
    };
    assert(rejected([&] { verify(loan_join); }));

    // Equal loan counts are not equal loan *identities*. A release at the
    // join is invalid when the two branches borrowed at different sites.
    Graph foreign_loan_join;
    foreign_loan_join.entry = {{"x", cell(kOwned)}};
    foreign_loan_join.blocks = {
        {{}, {1, 2}},
        {{{Kind::BorrowShared, "x"}}, {3}},
        {{{Kind::BorrowShared, "x"}}, {3}},
        {{{Kind::ReleaseShared, "x"}, {Kind::Move, "x"}}, {}}
    };
    assert(rejected([&] { verify(foreign_loan_join); }));

    // By contrast, a single common lexical loan can flow through two
    // branches and be released at their join without false conflicts.
    Graph common_loan_join;
    common_loan_join.entry = {{"x", cell(kOwned)}};
    common_loan_join.blocks = {
        {{{Kind::BorrowShared, "x"}}, {1, 2}},
        {{}, {3}},
        {{}, {3}},
        {{{Kind::ReleaseShared, "x"}, {Kind::Move, "x"}}, {}}
    };
    verify(common_loan_join);

    // Explicit tokens are checked for release authenticity as well.
    Graph foreign_release;
    foreign_release.entry = {{"x", cell(kOwned)}};
    foreign_release.blocks = {
        {{{Kind::BorrowShared, "x", 101}, {Kind::ReleaseShared, "x", 102}}, {}}
    };
    assert(rejected([&] { verify(foreign_release); }));
    foreign_release.blocks[0].operations[1].loan_id = 101;
    verify(foreign_release);

    // Two simultaneously borrowed shared loans need explicit release IDs.
    Graph anonymous_release;
    anonymous_release.entry = {{"x", cell(kOwned)}};
    anonymous_release.blocks = {
        {{{Kind::BorrowShared, "x", 1}, {Kind::BorrowShared, "x", 2},
          {Kind::ReleaseShared, "x"}}, {}}
    };
    assert(rejected([&] { verify(anonymous_release); }));
    anonymous_release.blocks[0].operations[2].loan_id = 1;
    anonymous_release.blocks[0].operations.push_back({Kind::ReleaseShared, "x", 2});
    verify(anonymous_release);

    // Drop with unknown ownership is an error unless the compiler emits
    // a runtime flag; the abstract flag-gated transfer remains explicit.
    Graph maybe_drop;
    maybe_drop.entry = {{"x", cell(kOwned)}};
    maybe_drop.blocks = {
        {{}, {1, 2}},
        {{{Kind::Move, "x"}}, {3}},
        {{}, {3}},
        {{{Kind::Drop, "x"}}, {}}
    };
    assert(rejected([&] { verify(maybe_drop); }));
    maybe_drop.blocks[3].operations = {op(Kind::DropIfOwned, "x")};
    const auto dropped = verify(maybe_drop);
    assert(dropped.outgoing[3]->at("x").possible == (kMoved | kDropped));

    // Malformed abstract states and contradictory loans must not be
    // accepted simply because the CFG has no reads or writes.
    Graph bad_entry;
    bad_entry.entry = {{"x", cell(static_cast<ValueMask>(kOwned | 0x80))}};
    bad_entry.blocks = {{{}, {}}};
    assert(rejected([&] { verify(bad_entry); }));
    bad_entry.entry.at("x").possible = 0;
    assert(rejected([&] { verify(bad_entry); }));
    bad_entry.entry.at("x") = {kOwned, 1, true};
    assert(rejected([&] { verify(bad_entry); }));
    // Loan provenance cannot coexist with maybe-moved or dropped places,
    // even when a later release would otherwise hide the illegal state.
    bad_entry.entry.at("x") = {static_cast<ValueMask>(kOwned | kMoved), 1, false};
    bad_entry.blocks = {{{op(Kind::ReleaseShared, "x")}, {}}};
    assert(rejected([&] { verify(bad_entry); }));
    bad_entry.entry.at("x") = {kDropped, 0, true};
    bad_entry.blocks = {{{op(Kind::ReleaseExclusive, "x")}, {}}};
    assert(rejected([&] { verify(bad_entry); }));
    bad_entry.entry.at("x") = {kOwned, 65536, false};
    bad_entry.blocks = {{{}, {}}};
    assert(rejected([&] { verify(bad_entry); }));
    bad_entry.entry.at("x") = {kOwned, 1, false};
    bad_entry.blocks = {{{op(Kind::ReleaseShared, "x"), op(Kind::Read, "x")}, {}}};
    verify(bad_entry);

    assert(rejected([&] {
        Graph invalid;
        invalid.entry = {{"x", cell(kOwned)}};
        invalid.blocks = {{{}, {2}}};
        verify(invalid);
    }));
    return 0;
}
