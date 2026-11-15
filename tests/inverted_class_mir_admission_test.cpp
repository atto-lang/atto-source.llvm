// Compile-time MIR admission checks only. This is not guest .atto class AOT.
#include "oreslang/types/InvertedClassMirAdmission.hpp"
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
using namespace oreslang::dop::class_mir;
using Ops = std::vector<SourceOperation>;

static SourceOperation make(Kind kind) {
    SourceOperation op{};
    op.kind = kind;
    op.acting_actor = 31;
    return op;
}
static Schema schema() {
    return {"workers", "Worker", "Particle", "x", "y", 31, 8};
}
static Ops valid() {
    Ops ops;
    ops.push_back(make(Kind::Allocate));
    auto push = make(Kind::Push);
    push.handle = "particle";
    ops.push_back(push);
    auto call = make(Kind::CallMoveBy);
    call.handle = "particle";
    call.receiver_class = "Particle";
    call.method = "move_by";
    call.mut_self = true;
    ops.push_back(call);
    auto read = make(Kind::ReadField);
    read.handle = "particle";
    read.field = "x";
    ops.push_back(read);
    auto snap = make(Kind::Snapshot);
    snap.handle = "particle";
    snap.snapshot_name = "saved";
    ops.push_back(snap);
    auto erase = make(Kind::EraseSwap);
    erase.handle = "particle";
    ops.push_back(erase);
    auto snapshot_read = make(Kind::ReadSnapshotField);
    snapshot_read.snapshot_name = "saved";
    snapshot_read.field = "y";
    ops.push_back(snapshot_read);
    ops.push_back(make(Kind::Close));
    return ops;
}
static void check(bool test, const char* why) {
    if (!test) throw std::runtime_error(why);
}
static void denied(const std::string& diagnostic, const std::function<void()>& f) {
    try {
        f();
    } catch (const std::exception& e) {
        if (std::string(e.what()).find(diagnostic) != std::string::npos) return;
        throw std::runtime_error(std::string("unexpected denial: ") + e.what());
    }
    throw std::runtime_error("MIR admission unexpectedly accepted unsafe program");
}
template<class F>
static void mutate_and_deny(const std::string& diagnostic, F change) {
    auto ops = valid();
    change(ops);
    denied(diagnostic, [&] { (void)VerifiedModule::lower(schema(), ops); });
}
int main() {
    const auto module = VerifiedModule::lower(schema(), valid());
    check(module.code().size() == 8, "missing MIR operations");
    check(module.code()[2].required.actor && module.code()[2].required.owner &&
          module.code()[2].required.generation && module.code()[2].required.bounds,
          "mutable receiver required runtime guards missing");
    check(module.code()[1].required.capacity, "push requires capacity guard");
    check(module.code()[2].required.arithmetic, "mutating mtd requires checked arithmetic");
    check(!module.code()[6].required.generation,
          "detached snapshot must not re-resolve stale row");
    (void)VerifiedModule::import_checked(schema(), module.code());

    // Independently imported MIR must reject any missing/forged guard.
    for (int kind = 0; kind < 6; ++kind) {
        auto copied = module.code();
        switch (kind) {
            case 0: copied[2].required.actor = false; break;
            case 1: copied[2].required.owner = false; break;
            case 2: copied[2].required.generation = false; break;
            case 3: copied[2].required.bounds = false; break;
            case 4: copied[1].required.capacity = false; break;
            case 5: copied[2].required.arithmetic = false; break;
        }
        denied("runtime guards", [&] { (void)VerifiedModule::import_checked(schema(), copied); });
    }
    mutate_and_deny("cross-actor", [](Ops& ops) { ops[2].acting_actor = 32; });
    mutate_and_deny("invalid mutable", [](Ops& ops) { ops[2].mut_self = false; });
    mutate_and_deny("invalid mutable", [](Ops& ops) { ops[2].receiver_class = "Other"; });
    mutate_and_deny("invalid mutable", [](Ops& ops) { ops[2].method = "unsafe_raw_ptr"; });
    mutate_and_deny("invalid inverted class field", [](Ops& ops) { ops[3].field = "secret"; });
    mutate_and_deny("stale inverted class handle", [](Ops& ops) {
        auto stale = make(Kind::ReadField);
        stale.handle = "particle";
        stale.field = "x";
        ops.insert(ops.end() - 1, stale);
    });
    mutate_and_deny("snapshot", [](Ops& ops) { ops[6].snapshot_name = "other"; });
    mutate_and_deny("snapshot", [](Ops& ops) { ops[4].snapshot_name = "particle"; });
    mutate_and_deny("not admitted", [](Ops& ops) {
        ops.insert(ops.end() - 1, make(Kind::Suspend));
    });
    mutate_and_deny("not admitted", [](Ops& ops) {
        ops.insert(ops.end() - 1, make(Kind::ExportHandle));
    });
    mutate_and_deny("terminal close", [](Ops& ops) { ops.pop_back(); });
    mutate_and_deny("handle binding reused", [](Ops& ops) {
        auto push = make(Kind::Push);
        push.handle = "particle";
        ops.insert(ops.end() - 1, push);
    });
    // A detached snapshot binding must reserve its name. A later handle
    // cannot silently shadow it even when it has never been issued before.
    mutate_and_deny("handle binding", [](Ops& ops) {
        auto push = make(Kind::Push);
        push.handle = "saved";
        ops.insert(ops.end() - 1, push);
    });
    auto wrong_schema = schema();
    wrong_schema.owner_actor = 0;
    denied("actor/capacity", [&] { (void)VerifiedModule::lower(wrong_schema, valid()); });
    wrong_schema = schema();
    wrong_schema.capacity = 0;
    denied("actor/capacity", [&] { (void)VerifiedModule::lower(wrong_schema, valid()); });
    wrong_schema = schema();
    wrong_schema.first_kind = ScalarKind::Bool;
    denied("only two i64", [&] { (void)VerifiedModule::lower(wrong_schema, valid()); });
    wrong_schema = schema();
    wrong_schema.second_kind = ScalarKind::Pointer;
    denied("only two i64", [&] { (void)VerifiedModule::lower(wrong_schema, valid()); });
    wrong_schema = schema();
    wrong_schema.second_field = "x";
    denied("duplicate inverted class field", [&] {
        (void)VerifiedModule::lower(wrong_schema, valid());
    });
    auto too_many = valid();
    for (std::size_t i = 0; i < 4100; ++i)
        too_many.insert(too_many.end() - 1, make(Kind::Allocate));
    denied("operation budget", [&] { (void)VerifiedModule::lower(schema(), too_many); });

    // Source and imported MIR are independently checked for guard presence.
    std::cout << "INVERTED_CLASS_TYPED_MIR_ADMISSION=PASS\n";
    return 0;
}
