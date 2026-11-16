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
    // The flat key space MUST NOT treat unproven indexed/deref projections
    // as independent storage. Otherwise a live loan on p can be bypassed by
    // a write through p[0] or through an encoded generated expression.
    for (const char* unproven : {"p[0]", "p[*]", "p->a", "p::a", "p.a[0]",
                                 "p.0", "p.$a", "p\\x", "p a"}) {
        Graph indexed_alias;
        indexed_alias.entry = {{"p", cell(kOwned)}, {unproven, cell(kOwned)}};
        indexed_alias.blocks = {{{op(Kind::BorrowShared, "p"),
                                  op(Kind::Move, unproven)}, {}}};
        assert(rejected([&] { verify(indexed_alias); }));
    }
    // Compiler-owned ANF root locals (main's $effectN) are flat places;
    // a bare or projected '$' is still rejected above / here.
    Graph generated_local;
    generated_local.entry = {{"$effect1", cell(kOwned)}};
    generated_local.blocks = {{{op(Kind::Read, "$effect1")}, {}}};
    verify(generated_local);
    for (const char* bad : {"$", "$.a", "$$x", "$1"}) {
        Graph bad_generated;
        bad_generated.entry = {{bad, cell(kOwned)}};
        bad_generated.blocks = {{{op(Kind::Read, bad)}, {}}};
        assert(rejected([&] { verify(bad_generated); }));
    }
    // A fieldless object's compiler-owned identity slot is a flat leaf place
    // (OwnershipChecker emits "<owner>.$identity"); nothing may project
    // through it and no other '$' leaf is admitted.
    Graph identity_slot;
    identity_slot.entry = {{"self.$identity", cell(kOwned)}};
    identity_slot.blocks = {{{op(Kind::Read, "self.$identity")}, {}}};
    verify(identity_slot);
    for (const char* bad : {"self.$identity.a", "self.$identityx", "self.$ident",
                            "self.a.$identity.b"}) {
        Graph bad_identity;
        bad_identity.entry = {{bad, cell(kOwned)}};
        bad_identity.blocks = {{{op(Kind::Read, bad)}, {}}};
        assert(rejected([&] { verify(bad_identity); }));
    }
    Graph too_deep_path;
    std::string deep_key = "p";
    for (int i = 0; i < 16; ++i) deep_key += ".a";
    too_deep_path.entry = {{deep_key, cell(kOwned)}};
    too_deep_path.blocks = {{{op(Kind::Read, deep_key.c_str())}, {}}};
    assert(rejected([&] { verify(too_deep_path); }));
    Graph huge_path;
    const std::string long_key(257, 'a');
    huge_path.entry = {{long_key, cell(kOwned)}};
    huge_path.blocks = {{{op(Kind::Read, long_key.c_str())}, {}}};
    assert(rejected([&] { verify(huge_path); }));
    // Valid, statically named sibling projections remain disjoint.
    Graph valid_named_siblings;
    valid_named_siblings.entry = {{"p.a", cell(kOwned)}, {"p.b", cell(kOwned)}};
    valid_named_siblings.blocks = {{{op(Kind::BorrowShared, "p.a"),
                                    op(Kind::Move, "p.b"),
                                    op(Kind::ReleaseShared, "p.a")}, {}}};
    verify(valid_named_siblings);

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

    // Direct operations must reject malformed state before mutation. An
    // explicit release ID previously still dereferenced an empty set because
    // optional::value_or evaluates its fallback eagerly.
    const auto reject_direct = [&](PlaceState malformed, Operation operation,
                                   const char* expected) {
        State state = {{"x", std::move(malformed)}};
        const State before = state;
        bool denied = false;
        try {
            apply(operation, state);
        } catch (const std::runtime_error& error) {
            denied = std::string(error.what()) ==
                std::string("ownership violation: ") + expected + " 'x'";
        }
        assert(denied);
        assert(state == before);
    };
    reject_direct({kOwned, 1, false}, {Kind::ReleaseShared, "x", 7},
        "unproven operation loan identity");
    reject_direct({kOwned, 1, false}, {Kind::ReleaseShared, "x"},
        "unproven operation loan identity");
    reject_direct({kOwned, 0, false, {7}}, {Kind::Move, "x"},
        "unproven operation loan identity");
    reject_direct({kOwned, 0, true}, {Kind::ReleaseExclusive, "x", 7},
        "unproven operation loan identity");
    reject_direct({kOwned, 0, false, {}, 7}, {Kind::Read, "x"},
        "unproven operation loan identity");
    reject_direct({kOwned, 1, true, {7}, 8}, {Kind::ReleaseShared, "x", 7},
        "conflicting operation borrow states");
    reject_direct({kMoved, 1, false, {7}}, {Kind::ReleaseShared, "x", 7},
        "live loan on non-owned operation place");
    reject_direct({kDropped, 0, true, {}, 7}, {Kind::ReleaseExclusive, "x", 7},
        "live loan on non-owned operation place");
    reject_direct({0}, {Kind::DropIfOwned, "x"},
        "invalid operation ownership mask");
    reject_direct({static_cast<ValueMask>(kOwned | 0x80)}, {Kind::DropIfOwned, "x"},
        "invalid operation ownership mask");

    // Valid direct loans and normalized legacy entry loans remain usable.
    State direct_shared = {{"x", {kOwned, 2, false, {7, 8}}}};
    apply({Kind::ReleaseShared, "x", 7}, direct_shared);
    apply({Kind::ReleaseShared, "x"}, direct_shared);
    apply({Kind::BorrowExclusive, "x", 9}, direct_shared);
    apply({Kind::ReleaseExclusive, "x", 9}, direct_shared);
    apply({Kind::Move, "x"}, direct_shared);
    assert(direct_shared.at("x").possible == kMoved);

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

    // Even a syntactically valid disconnected block must not escape the
    // ownership proof and become reachable after a compiler rewrite.
    Graph disconnected_valid;
    disconnected_valid.entry = {{"x", cell(kOwned)}};
    disconnected_valid.blocks = {{{op(Kind::Read, "x")}, {}},
                                 {{op(Kind::Move, "x")}, {}}};
    assert(rejected([&] { verify(disconnected_valid); }));

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
    // Executable native-C++ ownership CFG checks mapped to the shared
    // nine-toolchain borrow-mir-v1 logical case IDs. These use the actual
    // LLVM-side verifier, not the source-free JS oracle or emitted .atto guests.
    // C02/C04/C07/C08/C10-C14/C16/C20-C21 require typed MIR effects this
    // LLVM flat-place API cannot faithfully represent: DO NOT synthesize a pass.
    {
        Graph contract;
        contract.entry = {{"x", cell(kOwned)}};

        // C01: a definitely owned place is readable.
        contract.blocks = {{{op(Kind::Read, "x")}, {}}};
        verify(contract);

        // C03: use after a move must fail.
        contract.blocks = {{{op(Kind::Move, "x"), op(Kind::Read, "x")}, {}}};
        assert(rejected([&] { verify(contract); }));

        // C05: two independently identified shared loans can coexist.
        contract.blocks = {{{
            {Kind::BorrowShared, "x", 10},
            {Kind::BorrowShared, "x", 11},
            op(Kind::Read, "x"),
            {Kind::ReleaseShared, "x", 11},
            {Kind::ReleaseShared, "x", 10}
        }, {}}};
        verify(contract);

        // C06: an exclusive loan cannot overlap a live shared loan.
        contract.blocks = {{{
            {Kind::BorrowShared, "x", 10},
            {Kind::BorrowExclusive, "x", 11}
        }, {}}};
        assert(rejected([&] { verify(contract); }));

        // C09: a live shared loan fences destruction.
        contract.blocks = {{{
            {Kind::BorrowShared, "x", 10},
            op(Kind::Drop, "x")
        }, {}}};
        assert(rejected([&] { verify(contract); }));

        // C15: a nonexistent loan cannot be released.
        contract.blocks = {{{{Kind::ReleaseShared, "x", 10}}, {}}};
        assert(rejected([&] { verify(contract); }));

        // C22: a live shared loan cannot escape a terminal block.
        contract.blocks = {{{{Kind::BorrowShared, "x", 10}}, {}}};
        assert(rejected([&] { verify(contract); }));
    }
    {
        // C17: loan IDs are globally minted, not per branch.
        Graph reused_loan;
        reused_loan.entry = {{"x", cell(kOwned)}};
        reused_loan.blocks = {
            {{}, {1, 2}},
            {{{Kind::BorrowShared, "x", 10},
              {Kind::ReleaseShared, "x", 10}}, {3}},
            {{{Kind::BorrowShared, "x", 10},
              {Kind::ReleaseShared, "x", 10}}, {3}},
            {{{Kind::Read, "x"}}, {}}
        };
        assert(rejected([&] { verify(reused_loan); }));
    }
    {
        // C18: a MAY-moved owner at a CFG join cannot be read.
        Graph branch_moved;
        branch_moved.entry = {{"x", cell(kOwned)}};
        branch_moved.blocks = {
            {{}, {1, 2}},
            {{{Kind::Move, "x"}}, {3}},
            {{}, {3}},
            {{{Kind::Read, "x"}}, {}}
        };
        assert(rejected([&] { verify(branch_moved); }));
    }
    {
        // Backend extension, NOT shared C19 parity: the LLVM fixed-point
        // engine accepts a proven read-only cycle, while canonical Rust V0
        // deliberately rejects all cycles. Model this as capability drift
        // until a typed cyclic Rust contract is independently validated.
        Graph proven_read_loop;
        proven_read_loop.entry = {{"x", cell(kOwned)}};
        proven_read_loop.blocks = {
            {{}, {1}},
            {{{Kind::Read, "x"}}, {1, 2}},
            {{}, {}}
        };
        verify(proven_read_loop);
    }

    // Fail closed BEFORE fixed-point analysis or large allocations.
    // This guards the verifier itself against an adversarial typed-CFG
    // producer and does not claim that the source compiler is complete.
    Graph oversized_cfg;
    oversized_cfg.entry = {{"x", cell(kOwned)}};
    oversized_cfg.blocks.resize(kMaxCfgBlocks + 1);
    assert(rejected([&] { verify(oversized_cfg); }));

    // A massive declared state is itself untrusted input. Check place-count
    // limits before enumerating path aliases or starting fixed-point work.
    Graph oversized_places;
    oversized_places.blocks = {{{}, {}}};
    for (std::size_t i = 0; i <= kMaxCfgEntryPlaces; ++i)
        oversized_places.entry.emplace("place" + std::to_string(i), cell(kOwned));
    assert(rejected([&] { verify(oversized_places); }));

    Graph oversized_operations;
    oversized_operations.entry = oversized_cfg.entry;
    oversized_operations.blocks = {{{}, {}}};
    oversized_operations.blocks[0].operations.assign(
        kMaxCfgOperationsPerBlock + 1, op(Kind::Read, "x"));
    assert(rejected([&] { verify(oversized_operations); }));

    // The aggregate operation budget is independent of the per-block cap.
    // This graph stays under the 4096-block and 1024-op/block ceilings.
    Graph oversized_total_operations;
    oversized_total_operations.entry = oversized_cfg.entry;
    oversized_total_operations.blocks.resize(
        kMaxCfgOperations / kMaxCfgOperationsPerBlock + 1);
    for (auto& block : oversized_total_operations.blocks)
        block.operations.assign(kMaxCfgOperationsPerBlock, op(Kind::Read, "x"));
    assert(rejected([&] { verify(oversized_total_operations); }));

    Graph oversized_edges;
    oversized_edges.entry = oversized_cfg.entry;
    oversized_edges.blocks.resize(2);
    oversized_edges.blocks[0].successors.assign(kMaxCfgEdges + 1, 1);
    assert(rejected([&] { verify(oversized_edges); }));

    // Default-constructed operations have a deterministic, harmless enum
    // value, but their missing place still rejects, even under -DNDEBUG.
    Graph incomplete_operation;
    incomplete_operation.entry = {{"x", cell(kOwned)}};
    incomplete_operation.blocks = {{{Operation{}}, {}}};
    assert(rejected([&] { verify(incomplete_operation); }));

    // At the input limits, budgeted verification still accepts a safe
    // terminal graph. The limit is inclusive, not an off-by-one rejection.
    Graph bounded_graph;
    bounded_graph.entry = {{"x", cell(kOwned)}};
    bounded_graph.blocks = {{{op(Kind::Read, "x")}, {}}};
    verify(bounded_graph);

    return 0;
}
