#include "oreslang/runtime/ActorShardColumns.hpp"

#if defined(NDEBUG)
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

struct OresNativeActorScheduler { int marker; };
static thread_local OresNativeActorScheduler* current_scheduler = nullptr;
static thread_local OresNativeActorRef current_actor = {};
static bool mock_actor_done = false;

extern "C" int ores_native_actor_scheduler_wait_done(
        OresNativeActorScheduler* scheduler, OresNativeActorRef owner) {
    return mock_actor_done && scheduler == current_scheduler
        && owner.scheduler_domain == current_actor.scheduler_domain
        && owner.actor_slot == current_actor.actor_slot
        && owner.generation == current_actor.generation;
}

extern "C" int ores_native_actor_scheduler_is_current_actor(
        OresNativeActorScheduler* scheduler, OresNativeActorRef actor) {
    return scheduler == current_scheduler
        && actor.scheduler_domain == current_actor.scheduler_domain
        && actor.actor_slot == current_actor.actor_slot
        && actor.generation == current_actor.generation;
}

using oreslang::runtime::ActorShardBudget;
using oreslang::runtime::ActorShardColumns;
using oreslang::runtime::ActorColumnSnapshot;
using oreslang::dop::Layout;

int main() {
    OresNativeActorScheduler scheduler{1};
    OresNativeActorRef owner{1, 0, 8};
    current_scheduler = &scheduler;
    current_actor = owner;

    for (auto layout : {Layout::AoS, Layout::SoA, Layout::AoSoA}) {
        ActorShardBudget actor(1048576, &scheduler, owner);
        ActorShardBudget vm(2097152);
        {
            ActorShardColumns shard(
                &scheduler, owner, actor, vm, layout, 65, 2,
                layout == Layout::AoSoA ? 8 : 1);
            shard.write(64, 1, 200.0);
            const std::uint64_t shardBytes = actor.used_bytes();
            std::vector<std::unique_ptr<ActorColumnSnapshot>> copies;
            for (int i = 0; i < 1000; ++i)
                copies.push_back(shard.snapshot_column_leased(1));
            assert(actor.used_bytes() == shardBytes + 1000U * 65U * sizeof(double));
            assert(vm.used_bytes() == actor.used_bytes());
            assert(copies.back()->read(64) == 200.0);
            std::vector<double> copied(67, -1.0);
            copies.back()->copy_into(copied.data() + 1, 65);
            assert(copied.front() == -1.0 && copied.back() == -1.0);
            assert(copied[65] == 200.0);
            bool short_copy = false;
            try { copies.back()->copy_into(copied.data(), 64); }
            catch (const std::out_of_range&) { short_copy = true; }
            assert(short_copy);
            shard.write(64, 1, 300.0);
            assert(copies.back()->read(64) == 200.0);

            current_actor.generation++;
            bool foreign = false;
            try { (void)copies.back()->read(64); }
            catch (const std::logic_error&) { foreign = true; }
            assert(foreign);
            bool foreign_copy = false;
            try { copies.back()->copy_into(copied.data(), 65); }
            catch (const std::logic_error&) { foreign_copy = true; }
            assert(foreign_copy);
            bool foreign_close = false;
            try { copies.back()->close(); }
            catch (const std::logic_error&) { foreign_close = true; }
            assert(foreign_close);
            current_actor = owner;

            for (int i = 0; i < 500; ++i) {
                copies[i]->close();
                copies[i]->close();
                bool revoked = false;
                try { (void)copies[i]->read(0); }
                catch (const std::logic_error&) { revoked = true; }
                assert(revoked);
                bool revoked_copy = false;
                try { copies[i]->copy_into(copied.data(), 65); }
                catch (const std::logic_error&) { revoked_copy = true; }
                assert(revoked_copy);
            }
            assert(actor.used_bytes() == shardBytes + 500U * 65U * sizeof(double));
            copies.clear();
            assert(actor.used_bytes() == shardBytes);

            ActorShardBudget small(65U * sizeof(double), &scheduler, owner);
            {
                auto first = std::make_unique<ActorColumnSnapshot>(
                    &scheduler, owner, small, vm, 65);
                bool exhausted = false;
                try {
                    auto second = std::make_unique<ActorColumnSnapshot>(
                        &scheduler, owner, small, vm, 65);
                } catch (const std::length_error&) { exhausted = true; }
                assert(exhausted);
            }
            assert(small.used_bytes() == 0);
            auto outstanding = shard.snapshot_column_leased(1);
            assert(!oreslang::runtime::retire_actor_shard_budget_after_done(
                    &scheduler, owner, actor));
            assert(!actor.retired());
            mock_actor_done = true;
            assert(oreslang::runtime::retire_actor_shard_budget_after_done(
                    &scheduler, owner, actor));
            assert(actor.retired());
            assert(!actor.try_reserve(8));
            bool retired_read = false;
            try { (void)outstanding->read(0); }
            catch (const std::logic_error&) { retired_read = true; }
            assert(retired_read);
            bool retired_shard = false;
            try { (void)shard.read(64, 1); }
            catch (const std::logic_error&) { retired_shard = true; }
            assert(retired_shard);
            outstanding.reset(); // refunds even after actor stop
            mock_actor_done = false;
        }
        assert(actor.used_bytes() == 0 && vm.used_bytes() == 0);
    }
    return 0;
}
