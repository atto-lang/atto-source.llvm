#include "oreslang/types/OwnershipCfg.hpp"

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

    assert(rejected([&] {
        Graph invalid;
        invalid.entry = {{"x", cell(kOwned)}};
        invalid.blocks = {{{}, {2}}};
        verify(invalid);
    }));
    return 0;
}
