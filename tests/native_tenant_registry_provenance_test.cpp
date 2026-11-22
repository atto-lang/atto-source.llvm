#include "oreslang/runtime/NativeTenantAdmission.hpp"
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using oreslang::runtime::NativeTenantAdmission;

static void foreign_registry_handles() {
    NativeTenantAdmission left(2, 2, 32), right(1, 2, 32);
    const auto a = left.open(), b = right.open();
    const auto sibling = left.open();
    assert(a && b && sibling &&
           a->slot == b->slot && a->generation == b->generation &&
           sibling->slot != a->slot && sibling->generation == a->generation);
    // Historically either handle admitted the corresponding actor in the
    // wrong registry, because it carried only the (slot, generation) pair.
    assert(!left.enter(*b, 0));
    assert(!right.enter(*a, 0));
    assert(!left.request_stop(*b));
    assert(!right.request_stop(*a));
    assert(!left.finish_stop(*b));
    assert(!right.finish_stop(*a));
    auto retargeted = *a;
    // The modified public slot is valid and currently running in THIS VM.
    retargeted.slot = sibling->slot;
    assert(!left.enter(retargeted, 0));
    assert(!left.request_stop(retargeted));
    retargeted = *a;
    retargeted.generation = a->generation + 1;
    assert(!left.enter(retargeted, 0));
    assert(!left.request_stop(retargeted));
    const NativeTenantAdmission::Handle forged{a->slot, a->generation};
    assert(!left.enter(forged, 0));
    assert(!left.request_stop(forged));
    assert(!left.finish_stop(forged));
    auto live = left.enter(*a, 0), other = right.enter(*b, 0);
    assert(live && other);
    std::uint64_t word = 0x1122334455667788ULL, value = 1;
    assert(live->write(0, &word, sizeof(word)));
    assert(other->read(0, &value, sizeof(value)) && value == 0);
    live.reset(); other.reset();
    assert(left.request_stop(*a) && left.finish_stop(*a));
    assert(right.request_stop(*b) && right.finish_stop(*b));
    assert(left.request_stop(*sibling) && left.finish_stop(*sibling));
    const auto restart = left.open(); assert(restart);
    assert(restart->slot == a->slot && restart->generation != a->generation);
    // A stale issued handle cannot upgrade its publicly visible generation.
    retargeted = *a;
    retargeted.generation = restart->generation;
    assert(!left.enter(retargeted, 0));
    assert(!left.request_stop(retargeted));
    assert(!right.enter(*restart, 0));
    assert(!left.enter(*a, 0));
    assert(left.request_stop(*restart) && left.finish_stop(*restart));
}

static void premature_registry_destruction() {
    std::optional<NativeTenantAdmission::Turn> late;
    NativeTenantAdmission::Handle stale;
    {
        NativeTenantAdmission vm(1, 1, 16);
        auto h = vm.open(); assert(h);
        stale = *h;
        late = vm.enter(*h, 0); assert(late);
        const std::uint64_t marker = 77;
        assert(late->write(0, &marker, sizeof(marker)));
    }
    std::uint64_t value = 0;
    assert(late);
    assert(!late->read(0, &value, sizeof(value)));
    assert(!late->write(0, &value, sizeof(value)));
    NativeTenantAdmission next(1, 1, 16);
    const auto fresh = next.open(); assert(fresh);
    assert(!next.enter(stale, 0));
    assert(!next.request_stop(stale));
    assert(!next.finish_stop(stale));
    late.reset();
    auto turn = next.enter(*fresh, 0); assert(turn);
    assert(turn->read(0, &value, sizeof(value)) && value == 0);
    turn.reset();
    assert(next.request_stop(*fresh) && next.finish_stop(*fresh));
}

static void independent_vm_concurrency() {
    constexpr int kRegistries = 16;
    std::vector<std::unique_ptr<NativeTenantAdmission>> vms;
    std::vector<NativeTenantAdmission::Handle> handles;
    for (int i = 0; i < kRegistries; ++i) {
        vms.emplace_back(new NativeTenantAdmission(1, 1, 16));
        const auto h = vms.back()->open(); assert(h);
        handles.push_back(*h);
    }
    std::atomic<bool> begin{false};
    std::atomic<int> good{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < kRegistries; ++i) {
        threads.emplace_back([&, i] {
            while (!begin.load(std::memory_order_acquire)) std::this_thread::yield();
            for (int j = 0; j < kRegistries; ++j) {
                if (i == j) continue;
                assert(!vms[i]->enter(handles[j], 0));
                assert(!vms[i]->request_stop(handles[j]));
            }
            auto turn = vms[i]->enter(handles[i], 0); assert(turn);
            std::uint64_t marker = static_cast<std::uint64_t>(i+1);
            assert(turn->write(0, &marker, sizeof(marker)));
            good.fetch_add(1);
        });
    }
    begin.store(true, std::memory_order_release);
    for (auto& t : threads) t.join();
    assert(good.load() == kRegistries);
    for (int i = 0; i < kRegistries; ++i)
        assert(vms[i]->request_stop(handles[i]) && vms[i]->finish_stop(handles[i]));
}

int main() {
    foreign_registry_handles();
    premature_registry_destruction();
    independent_vm_concurrency();
}
