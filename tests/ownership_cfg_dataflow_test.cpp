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

    // A flat place map has no ancestor/descendant alias proof. Reject a
    // whole owner and one of its projections being independently tracked:
    // otherwise an exclusive loan on one could be bypassed through the other.
    Graph alias_layout;
    alias_layout.entry = {{"p", cell(kOwned)}, {"p.a", cell(kOwned)}};
    alias_layout.blocks = {{{op(Kind::BorrowExclusive, "p"),
                            op(Kind::Read, "p.a"),
                            op(Kind::ReleaseExclusive, "p")}, {}}};
    assert(rejected([&] { verify(alias_layout); }));
    alias_layout.blocks = {{{}, {}}}; // Fail closed even without a read.
    assert(rejected([&] { verify(alias_layout); }));
    alias_layout.blocks = {{{op(Kind::BorrowExclusive, "p.a"),
                            op(Kind::Read, "p")}, {}}};
    assert(rejected([&] { verify(alias_layout); }));

    // Nested parent/subfield projections are just as overlapping as direct
    // parent/field pairs. Prefix similarity without a '.' is NOT overlap.
    alias_layout.entry = {{"p.a", cell(kOwned)}, {"p.a.deep", cell(kOwned)}};
    alias_layout.blocks = {{{}, {}}};
    assert(rejected([&] { verify(alias_layout); }));
    alias_layout.entry = {{"p.a", cell(kOwned)}, {"p.ab", cell(kOwned)}};
    alias_layout.blocks = {{{op(Kind::Read, "p.a"), op(Kind::Read, "p.ab")}, {}}};
    verify(alias_layout);
    alias_layout.entry = {{"p.a", cell(kOwned)}, {"p.b", cell(kOwned)}};
    alias_layout.blocks = {{{op(Kind::BorrowExclusive, "p.a"),
                            op(Kind::Read, "p.b"),
                            op(Kind::ReleaseExclusive, "p.a"),
                            op(Kind::ReadObject, "p")}, {}}};
    verify(alias_layout);

    // Direct apply() must reject ambiguous storage too, even without verify().
    State direct_alias = {{"p", cell(kOwned)}, {"p.a", cell(kOwned)}};
    assert(rejected([&] { apply(op(Kind::Read, "p.a"), direct_alias); }));
    assert(rejected([&] { apply(op(Kind::MoveObject, "p"), direct_alias); }));
    Graph invalid_name;
    invalid_name.entry = {{"p..a", cell(kOwned)}};
    invalid_name.blocks = {{{}, {}}};
    assert(rejected([&] { verify(invalid_name); }));

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

    // A forged lexical token reused by two different borrow sites must
    // be rejected even when only one branch executes. Equal sets at the
    // join cannot prove identical borrowed identities in that case.
    Graph forged_same_id = foreign_loan_join;
    forged_same_id.blocks[1].operations[0].loan_id = 77;
    forged_same_id.blocks[2].operations[0].loan_id = 77;
    assert(rejected([&] { verify(forged_same_id); }));
    // Borrow IDs must not collide even when the loans target distinct places.
    Graph different_places;
    different_places.entry = {{"a", cell(kOwned)}, {"b", cell(kOwned)}};
    different_places.blocks = {{
        {{Kind::BorrowShared, "a", 90}, {Kind::BorrowExclusive, "b", 90}},
        {}
    }};
    assert(rejected([&] { verify(different_places); }));

    // Explicit IDs must not impersonate implicitly derived site IDs.
    Graph impersonated_site;
    impersonated_site.entry = {{"a", cell(kOwned)}, {"b", cell(kOwned)}};
    impersonated_site.blocks = {{
        {{Kind::BorrowShared, "a"},
         {Kind::BorrowShared, "b", (static_cast<std::uint64_t>(1) << 32) | 1}},
        {}
    }};
    assert(rejected([&] { verify(impersonated_site); }));

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

    // Token zero is the unique legacy entry-loan identity, not a value
    // that an independently executed branch may mint.
    Graph forged_entry;
    forged_entry.entry = {{"x", cell(kOwned)}};
    forged_entry.blocks = {{{op(Kind::BorrowShared, "x"), op(Kind::ReleaseShared, "x")}, {}}};
    forged_entry.blocks[0].operations[0].loan_id = 0;
    assert(rejected([&] { verify(forged_entry); }));
    forged_entry.blocks[0].operations = {
        {Kind::BorrowExclusive, "x", 0}, {Kind::ReleaseExclusive, "x", 0}
    };
    assert(rejected([&] { verify(forged_entry); }));

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

    // A legacy entry loan must not impersonate another lexical borrow.
    // Nonzero entry tokens must remain unique across all places and
    // distinct from both explicit and implicit compiler site tokens.
    Graph entry_vs_lexical;
    entry_vs_lexical.entry = {{"a", cell(kOwned)}, {"b", cell(kOwned)}};
    entry_vs_lexical.entry.at("a").shared_loans = 1;
    entry_vs_lexical.entry.at("a").shared_loan_ids.insert(77);
    entry_vs_lexical.blocks = {{
        {{Kind::BorrowShared, "b", 77},
         {Kind::ReleaseShared, "b", 77},
         {Kind::ReleaseShared, "a", 77}}, {}
    }};
    assert(rejected([&] { verify(entry_vs_lexical); }));

    // A generated site ID must not alias a live parameter loan.
    entry_vs_lexical.blocks[0].operations[0].loan_id.reset();
    entry_vs_lexical.blocks[0].operations[1].loan_id =
            (static_cast<std::uint64_t>(1) << 32) | 1;
    entry_vs_lexical.entry.at("a").shared_loan_ids.clear();
    entry_vs_lexical.entry.at("a").shared_loan_ids.insert(
            (static_cast<std::uint64_t>(1) << 32) | 1);
    entry_vs_lexical.blocks[0].operations[2].loan_id =
            (static_cast<std::uint64_t>(1) << 32) | 1;
    assert(rejected([&] { verify(entry_vs_lexical); }));

    // Two borrowed parameters must not claim the same nonzero identity.
    entry_vs_lexical.entry.at("a").shared_loan_ids.clear();
    entry_vs_lexical.entry.at("a").shared_loan_ids.insert(89);
    entry_vs_lexical.entry.at("b").shared_loans = 1;
    entry_vs_lexical.entry.at("b").shared_loan_ids.insert(89);
    entry_vs_lexical.blocks = {{{}, {}}};
    assert(rejected([&] { verify(entry_vs_lexical); }));

    // Anonymous token zero is permitted only for the legacy single entry
    // loan, never when mixed with multiple named entry loans.
    entry_vs_lexical.entry.at("b").shared_loans = 0;
    entry_vs_lexical.entry.at("b").shared_loan_ids.clear();
    entry_vs_lexical.entry.at("a").shared_loans = 2;
    entry_vs_lexical.entry.at("a").shared_loan_ids = {0, 89};
    assert(rejected([&] { verify(entry_vs_lexical); }));

    // Anonymous entry identity 0 represents one legacy loan, not one loan
    // per parameter. Two distinct parameter loans must carry unique IDs.
    Graph duplicate_entry_zero;
    duplicate_entry_zero.entry = {{"a", {kOwned, 1, false}},
                                  {"b", {kOwned, 1, false}}};
    duplicate_entry_zero.blocks = {{{op(Kind::ReleaseShared, "a"),
                                     op(Kind::ReleaseShared, "b")}, {}}};
    assert(rejected([&] { verify(duplicate_entry_zero); }));
    duplicate_entry_zero.entry.at("a").shared_loan_ids = {401};
    duplicate_entry_zero.entry.at("b").shared_loan_ids = {402};
    verify(duplicate_entry_zero);
    duplicate_entry_zero.entry.at("b") = {kOwned, 0, true};
    duplicate_entry_zero.blocks = {{{op(Kind::ReleaseShared, "a"),
                                     op(Kind::ReleaseExclusive, "b")}, {}}};
    verify(duplicate_entry_zero);
    duplicate_entry_zero.entry.at("a").shared_loan_ids.clear();
    assert(rejected([&] { verify(duplicate_entry_zero); }));

    // Direct helper calls are not CFG sites and cannot mint token zero.
    State direct = {{"x", cell(kOwned)}};
    assert(rejected([&] { apply(op(Kind::BorrowShared, "x"), direct); }));
    assert(rejected([&] { apply(op(Kind::BorrowExclusive, "x"), direct); }));
    assert(direct.at("x").shared_loans == 0 &&
           !direct.at("x").exclusive_loan);

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

    // Verify every CFG successor, not only edges reached by the worklist.
    // An invalid edge in a currently unreachable block must not survive
    // into native emission or become reachable after a later transform.
    Graph unreachable_bad_edge;
    unreachable_bad_edge.entry = {{"x", cell(kOwned)}};
    unreachable_bad_edge.blocks = {{{}, {}}, {{}, {99}}};
    assert(rejected([&] { verify(unreachable_bad_edge); }));

    assert(rejected([&] {
        Graph invalid;
        invalid.entry = {{"x", cell(kOwned)}};
        invalid.blocks = {{{}, {2}}};
        verify(invalid);
    }));
    return 0;
}
