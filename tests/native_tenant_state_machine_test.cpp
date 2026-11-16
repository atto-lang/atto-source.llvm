#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

using oreslang::runtime::NativeTenantAdmission;

static std::uint64_t seed = 0x37798d72b5c3e1a1ULL;
static std::uint64_t random_word() noexcept {
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return seed * 2685821657736338717ULL;
}

struct TenantModel {
    std::optional<NativeTenantAdmission::Handle> handle;
    int phase = 0;  // 0 stopped, 1 running, 2 quiescing
    std::array<std::optional<NativeTenantAdmission::Turn>, 3> turns;
    std::array<std::uint64_t, 3> expected{};
};
struct VmModel {
    NativeTenantAdmission vm{4, 3, 16};
    std::array<TenantModel, 4> tenants;
};

int main() {
    constexpr int kVms = 4, kSteps = 30000;
    std::array<std::unique_ptr<VmModel>, kVms> worlds;
    for (auto& world : worlds) world = std::make_unique<VmModel>();
    unsigned opens = 0, turns = 0, writes = 0, stops = 0, foreign = 0;
    for (int iteration = 0; iteration < kSteps; ++iteration) {
        const std::size_t v = random_word() % kVms;
        const std::size_t t = random_word() % 4;
        const std::size_t actor = random_word() % 3;
        const unsigned op = static_cast<unsigned>(random_word() % 10);
        auto& world = *worlds[v];
        auto& model = world.tenants[t];
        if (op == 0) {
            const auto admitted = world.vm.open();
            int expected_slot = -1;
            for (int j = 0; j < 4; ++j) {
                if (world.tenants[j].phase == 0) {
                    expected_slot = j;
                    break;
                }
            }
            assert(bool(admitted) == (expected_slot >= 0));
            if (admitted) {
                assert(admitted->slot == static_cast<std::size_t>(expected_slot));
                auto& admitted_model = world.tenants[admitted->slot];
                assert(!admitted_model.handle ||
                       admitted->generation > admitted_model.handle->generation);
                admitted_model.handle = *admitted;
                admitted_model.phase = 1;
                for (auto& expected : admitted_model.expected) expected = 0;
                ++opens;
            }
        } else if (op == 1 && model.handle) {
            auto turn = world.vm.enter(*model.handle, actor);
            const bool should_succeed = model.phase == 1 && !model.turns[actor];
            assert(bool(turn) == should_succeed);
            if (turn) {
                model.turns[actor] = std::move(*turn);
                ++turns;
            }
        } else if (op == 2 && model.turns[actor]) {
            const std::uint64_t value = random_word();
            assert(model.turns[actor]->write(0, &value, sizeof(value)));
            model.expected[actor] = value;
            ++writes;
        } else if (op == 3 && model.turns[actor]) {
            std::uint64_t value = std::numeric_limits<std::uint64_t>::max();
            assert(model.turns[actor]->read(0, &value, sizeof(value)));
            assert(value == model.expected[actor]);
        } else if (op == 4 && model.handle) {
            const bool result = world.vm.request_stop(*model.handle);
            assert(result == (model.phase == 1));
            if (result) { model.phase = 2; ++stops; }
        } else if (op == 5 && model.handle) {
            const bool no_turns = !model.turns[0] &&
                                  !model.turns[1] && !model.turns[2];
            const bool should_finish = model.phase == 2 && no_turns;
            const bool finished = world.vm.finish_stop(*model.handle);
            assert(finished == should_finish);
            if (finished) {
                model.phase = 0;
                for (auto& expected : model.expected) expected = 0;
                assert(!world.vm.enter(*model.handle, actor));
            }
        } else if (op == 6) {
            model.turns[actor].reset();
        } else if (op == 7) {
            auto& neighbor = *worlds[(v + 1) % kVms];
            for (const auto& candidate : neighbor.tenants) {
                if (!candidate.handle) continue;
                assert(!world.vm.enter(*candidate.handle, actor));
                assert(!world.vm.request_stop(*candidate.handle));
                assert(!world.vm.finish_stop(*candidate.handle));
                ++foreign;
            }
        } else if (op == 8 && model.handle) {
            auto tampered = *model.handle;
            tampered.slot = (tampered.slot + 1) % 4;
            assert(!world.vm.enter(tampered, actor));
            assert(!world.vm.request_stop(tampered));
            tampered = *model.handle;
            ++tampered.generation;
            assert(!world.vm.enter(tampered, actor));
        } else if (op == 9 && model.turns[actor]) {
            std::uint64_t value = 0;
            assert(!model.turns[actor]->read(15, &value, sizeof(value)));
            assert(!model.turns[actor]->write(16, &value, sizeof(value)));
        }
    }
    for (auto& world : worlds) {
        for (auto& model : world->tenants) {
            for (auto& turn : model.turns) turn.reset();
            if (model.phase == 0) continue;
            assert(model.handle);
            if (model.phase == 1) assert(world->vm.request_stop(*model.handle));
            assert(world->vm.finish_stop(*model.handle));
        }
    }
    // Guard against accidentally reducing a test to a trivial oracle walk.
    assert(opens > 25 && turns > 50 && writes > 25);
    assert(stops > 25 && foreign > 25);
}
