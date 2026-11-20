#include "oreslang/types/OwnershipDropPlan.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>
#include <cstdio>

using namespace oreslang::ownership;
using Kind = Operation::Kind;

static void check(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

template<class F>
static void expect_reject(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::runtime_error& e) {
        rejected = std::string(e.what()).find("ownership violation:") != std::string::npos;
    }
    check(rejected, "expected ownership rejection");
}

struct Token {
    int id;
    std::vector<int>* destroyed;
};
static void drop_token(void* raw) noexcept {
    auto* value = static_cast<Token*>(raw);
    value->destroyed->push_back(value->id);
    delete value;
}
static DropSlot live(int id, std::vector<int>& log) {
    return {kOwned, new Token{id, &log}, drop_token};
}

static void diamond_partial_init() {
    Graph graph;
    graph.entry = {{"p.a", {kUninitialized, 0, false}},
                   {"p.b", {kOwned, 0, false}}};
    graph.blocks = {
        {{}, {1, 2}},
        {{{Kind::Initialize, "p.a"}}, {3}},
        {{}, {3}},
        {{}, {}}
    };
    auto plans = plan_drop_exits(graph, {"p.a", "p.b"});
    check(plans.size() == 1 && plans[0].block_index == 3, "diamond exit");
    check(plans[0].fields.size() == 2, "field count");
    check(plans[0].fields[0].place == "p.b", "reverse order");
    check(plans[0].fields[1].possible == (kOwned | kUninitialized), "joined partial state");

    for (bool took_init : {false, true}) {
        std::vector<int> log;
        DropSlots slots{{"p.a", took_init ? live(1, log) : DropSlot{}},
                        {"p.b", live(2, log)}};
        run_drop_exit(plans[0], slots);
        check(log == (took_init ? std::vector<int>{2, 1} : std::vector<int>{2}),
              "partial-init drop order");
        const auto before = log.size();
        expect_reject([&] { run_drop_exit(plans[0], slots); });
        check(log.size() == before, "double destructor invocation");
    }
}

static void partial_move_and_prior_drop() {
    for (Kind effect : {Kind::Move, Kind::Drop}) {
        Graph graph;
        graph.entry = {{"record.left", {kOwned, 0, false}},
                       {"record.right", {kOwned, 0, false}}};
        graph.blocks = {
            {{}, {1, 2}},
            {{{effect, "record.left"}}, {3}},
            {{}, {3}},
            {{}, {}}
        };
        const auto plans = plan_drop_exits(graph, {"record.left", "record.right"});
        for (bool first_edge : {false, true}) {
            std::vector<int> log;
            DropSlots slots{{"record.left", first_edge ? DropSlot{effect == Kind::Move ? kMoved : kDropped, nullptr, nullptr} : live(1, log)},
                            {"record.right", live(2, log)}};
            run_drop_exit(plans.at(0), slots);
            check(log == (first_edge ? std::vector<int>{2} : std::vector<int>{2, 1}),
                  "moved or already dropped field should not drop twice");
        }
    }
}

static void wrong_flag_preflights_before_any_drop() {
    Graph graph;
    graph.entry = {{"x", {kOwned, 0, false}}, {"y", {kOwned, 0, false}}};
    graph.blocks = {{{}, {}}};
    auto plans = plan_drop_exits(graph, {"x", "y"});
    std::vector<int> log;
    DropSlots slots{{"x", {kMoved, nullptr, nullptr}}, {"y", live(2, log)}};
    expect_reject([&] { run_drop_exit(plans.at(0), slots); });
    check(log.empty() && slots.at("y").state == kOwned,
          "preflight must reject before first destructor");
    // Clean test-owned native resource explicitly after rejection.
    drop_token(slots.at("y").address);
}

static void reject_alias_before_destructors() {
    Graph graph;
    graph.entry = {{"a", {kOwned, 0, false}}, {"b", {kOwned, 0, false}}};
    graph.blocks = {{{}, {}}};
    const auto plans = plan_drop_exits(graph, {"a", "b"});
    std::vector<int> log;
    auto* shared = new Token{1, &log};
    DropSlots slots{{"a", {kOwned, shared, drop_token}},
                    {"b", {kOwned, shared, drop_token}}};
    expect_reject([&] { run_drop_exit(plans.at(0), slots); });
    check(log.empty() && slots.at("a").state == kOwned &&
          slots.at("b").state == kOwned, "duplicate owned pointer must not run destructors");
    delete shared; // Only one native allocation was created.
}

static void reject_stale_capabilities_and_bad_masks() {
    Graph graph;
    graph.entry = {{"a", {static_cast<ValueMask>(kMoved | kDropped | kUninitialized), 0, false}},
                   {"b", {kOwned, 0, false}}};
    graph.blocks = {{{}, {}}};
    const auto plans = plan_drop_exits(graph, {"a", "b"});
    std::vector<int> log;
    auto* stale = new Token{1, &log};
    for (ValueMask state : {kMoved, kDropped, kUninitialized}) {
        DropSlots slots{{"a", {state, stale, drop_token}},
                        {"b", live(2, log)}};
        expect_reject([&] { run_drop_exit(plans.at(0), slots); });
        check(log.empty() && slots.at("b").state == kOwned,
              "stale native capability must fail before destructor");
        delete static_cast<Token*>(slots.at("b").address);
    }
    delete stale;
    DropSlots valid{{"a", {kMoved, nullptr, nullptr}},
                    {"b", live(2, log)}};
    DropExit forged = plans.at(0);
    forged.fields.at(0).possible = static_cast<ValueMask>(kOwned | 0x80);
    expect_reject([&] { run_drop_exit(forged, valid); });
    check(log.empty() && valid.at("b").state == kOwned,
          "invalid abstract state must fail before destructor");
    delete static_cast<Token*>(valid.at("b").address);
}

static void reject_missing_extra_or_duplicate_drop_places() {
    Graph graph;
    graph.entry = {{"a", {kOwned, 0, false}}, {"b", {kOwned, 0, false}}};
    graph.blocks = {{{}, {}}};
    const auto plans = plan_drop_exits(graph, {"a", "b"});
    std::vector<int> log;
    DropSlots slots{{"a", live(1, log)}, {"b", live(2, log)}};
    DropExit forged = plans.at(0);
    forged.fields.at(0).place = "a";
    expect_reject([&] { run_drop_exit(forged, slots); });
    check(log.empty(), "duplicate cleanup place must not run destructor");
    forged = plans.at(0);
    forged.fields.at(1).place = "unknown";
    expect_reject([&] { run_drop_exit(forged, slots); });
    check(log.empty(), "missing field must not run destructor");
    DropSlots extra = slots;
    extra.emplace("stray", DropSlot{kUninitialized, nullptr, nullptr});
    expect_reject([&] { run_drop_exit(plans.at(0), extra); });
    check(log.empty(), "extra drop slot must not be silently ignored");
    delete static_cast<Token*>(slots.at("a").address);
    delete static_cast<Token*>(slots.at("b").address);
}

static void forbid_live_loans_and_invalid_layout() {
    Graph graph;
    graph.entry = {{"x", {kOwned, 1, false}}};
    graph.blocks = {{{}, {}}};
    expect_reject([&] { (void)plan_drop_exits(graph, {"x"}); });
    graph.entry.at("x").shared_loans = 0;
    expect_reject([&] { (void)plan_drop_exits(graph, {"x", "x"}); });
}

int main() {
    diamond_partial_init();
    partial_move_and_prior_drop();
    wrong_flag_preflights_before_any_drop();
    reject_alias_before_destructors();
    reject_stale_capabilities_and_bad_masks();
    reject_missing_extra_or_duplicate_drop_places();
    forbid_live_loans_and_invalid_layout();
    std::puts("NATIVE_CFG_EXACT_ONCE_FIELD_DROP_OK");
}
