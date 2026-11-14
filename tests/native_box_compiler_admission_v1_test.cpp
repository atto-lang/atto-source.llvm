#include "oreslang/types/NativeBoxCompilerAdmissionV1.hpp"

#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace oreslang::types::native_box_v1;
using E = EventKind;

#define REQUIRE(v) do { if (!(v)) throw std::runtime_error("failed: " #v); } while (false)

static Plan plan(std::vector<std::string> places, std::vector<Event> events,
                 bool trusted = true) {
    return Plan{trusted, std::move(places), std::move(events)};
}
static void reject(const Plan& value) {
    bool failed = false;
    try { (void)check(value); }
    catch (const std::runtime_error&) { failed = true; }
    REQUIRE(failed);
}
int main() {
    // An owned Box remaining at scope exit must be included in exact-once
    // compiler drop planning. A taken Box has no destructor in source frame.
    {
        auto proof = check(plan({"box"}, {{E::Initialize,"box"},{E::Read,"box"}}));
        REQUIRE(proof.exit.fields.size() == 1);
        REQUIRE(proof.exit.fields.front().place == "box");
        REQUIRE(proof.exit.fields.front().possible == oreslang::ownership::kOwned);
    }
    {
        auto proof = check(plan({"box","new_owner"},
            {{E::Initialize,"box"},{E::TakeInto,"box","new_owner"}}));
        REQUIRE(proof.terminal.at("box").possible == oreslang::ownership::kMoved);
        REQUIRE(proof.terminal.at("new_owner").possible == oreslang::ownership::kOwned);
        REQUIRE(proof.exit.fields.size() == 2);
        REQUIRE(proof.exit.fields[0].place == "new_owner"); // reverse declaration order
    }
    {
        auto proof = check(plan({"box"}, {
            {E::Initialize,"box"},
            {E::BorrowRead,"box","",101},
            {E::Read,"box"},
            {E::ReleaseRead,"box","",101},
            {E::BorrowWrite,"box","",102},
            {E::ReleaseWrite,"box","",102},
            {E::Drop,"box"}
        }));
        REQUIRE(proof.terminal.at("box").possible == oreslang::ownership::kDropped);
    }
    {
        auto proof = check(plan({"box"}, {
            {E::Initialize,"box"}, {E::ThreadMove,"box","",0,true}
        }));
        REQUIRE(proof.terminal.at("box").possible == oreslang::ownership::kMoved);
    }

    // No user-defined "Box" class, borrowed move, implicit copy or live
    // frame across await/select is permitted to claim deterministic cleanup.
    reject(plan({"box"}, {{E::Initialize,"box"}}, false));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::Copy,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::Read,"box"},
                          {E::TakeInto,"box","box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::TakeInto,"box","missing"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::Drop,"box"},
                          {E::Read,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::ThreadMove,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::ActorSend,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::Suspend,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::BorrowRead,"box","",5},
                          {E::TakeInto,"box","other"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::BorrowWrite,"box","",5},
                          {E::Read,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::BorrowRead,"box","",5},
                          {E::BorrowWrite,"box","",6}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::BorrowRead,"box","",5},
                          {E::ReleaseRead,"box","",6}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::BorrowRead,"box","",5}}));
    reject(plan({"box"}, {{E::Initialize,"box"},{E::Initialize,"box"}}));
    reject(plan({"box"}, {{E::Drop,"box"}}));
    reject(plan({"box","box"}, {}));
    reject(plan({"box","box.field"}, {}));
    reject(plan({"box"}, {{E::Initialize,"box"},
                          {E::ThreadMove,"box","",0,true},
                          {E::Drop,"box"}}));
    reject(plan({"box"}, {{E::Initialize,"box"},
                          {E::BorrowRead,"box","",1},
                          {E::BorrowRead,"box","",1}}));
    // Bounding prevents attacker-crafted plans from consuming unbounded
    // checker CPU/stack, independently from parsing.
    auto excessive = plan({"box"}, {});
    excessive.events.resize(4097, {E::Read,"box"});
    reject(excessive);
    std::puts("ATTO_NATIVE_BOX_COMPILER_ADMISSION_V1_PASS");
}
