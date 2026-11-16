// Host-only native contract; not source-compiled Oreslang actor execution.
#include "oreslang/runtime/NativeBatchPersistence.hpp"
#include <cassert>
#include <iostream>
#include <thread>
#include <atomic>
using namespace oreslang::runtime::persistence;

static Write write(std::string actor, std::string op, std::uint64_t epoch = 1,
                   std::uint64_t expected = 0) {
    return {{"tenant-a",std::move(actor),"main"},std::move(op),epoch,expected,
        1,WriteKind::Snapshot,{42}};
}
int main() {
    { // 1,000 actor states grouped in 256-entry batches.
        MemoryStore store;
        BatchSidecar sidecar("tenant-a", store, 2000, 1 << 20, 1024, 256, 64 * 1024);
        for (unsigned i = 0; i < 1000; ++i) {
            const auto actor = "actor-" + std::to_string(i);
            store.grant_epoch("tenant-a",actor,1);
            sidecar.enqueue(write(actor,"op-"+std::to_string(i)));
        }
        std::size_t committed = 0;
        while (sidecar.queued_count()) {
            for (auto& r : sidecar.flush()) { assert(r.applied() && !r.durable()); ++committed; }
        }
        assert(committed == 1000 && store.commit_calls() == 4);
        assert(store.version({"tenant-a","actor-999","main"}) == 1);
    }
    { // Activation fencing happens in provider commit, not admission.
        MemoryStore store;store.grant_epoch("tenant-a","a",1);
        BatchSidecar sidecar("tenant-a",store,16,4096,16,16,64);
        sidecar.enqueue(write("a","stale",1));
        store.grant_epoch("tenant-a","a",2);
        assert(sidecar.flush()[0].status == Status::StaleEpoch);
        sidecar.enqueue(write("a","fresh",2));
        assert(sidecar.flush()[0].status == Status::Committed);
        assert(store.version({"tenant-a","a","main"}) == 1);
    }
    { // Cross-actor CAS isolation, dedupe and altered op-id rejection.
        MemoryStore store;
        store.grant_epoch("tenant-a","a",1);store.grant_epoch("tenant-a","b",1);
        BatchSidecar sidecar("tenant-a",store,16,4096,16,16,64);
        sidecar.enqueue(write("a","x"));sidecar.enqueue(write("a","y"));
        sidecar.enqueue(write("b","z"));
        auto r=sidecar.flush();
        assert(r.size()==3 && r[0].status==Status::Committed &&
             r[1].status==Status::VersionConflict && r[2].status==Status::Committed);
        sidecar.enqueue(write("a","x"));
        assert(sidecar.flush()[0].status==Status::Duplicate);
        auto altered=write("a","x");altered.payload={43};sidecar.enqueue(altered);
        assert(sidecar.flush()[0].status==Status::OperationConflict);
    }
    { // No cross-tenant authorization or use of source actor buffers after admission.
        MemoryStore store;store.grant_epoch("tenant-a","a",1);
        BatchSidecar sidecar("tenant-a",store,16,4096,16,16,64);
        auto x=write("a","copied");sidecar.enqueue(x);x.payload[0]=9;
        { auto r=sidecar.flush()[0]; assert(r.applied() && !r.durable()); }
        assert(store.snapshot({"tenant-a","a","main"})[0] == 42);
        bool rejected=false;
        try { auto y=write("a","forged"); y.id.tenant="tenant-b"; sidecar.enqueue(y); }
        catch (const std::logic_error&) { rejected=true; }
        assert(rejected);
    }
    { // Zero-byte flood bounded by queue slots/bytes.
        MemoryStore store;store.grant_epoch("tenant-a","a",1);
        BatchSidecar sidecar("tenant-a",store,2,256,16,4,64);
        auto x=write("a","one");x.payload.clear();sidecar.enqueue(x);
        x.operation_id="two";sidecar.enqueue(x);
        bool rejected=false;
        try { x.operation_id="three";sidecar.enqueue(x); }
        catch (const std::length_error&) { rejected=true; }
        assert(rejected);
    }
    { // The old generation may not replay a successful receipt after fencing.
        MemoryStore store;store.grant_epoch("tenant-a","a",1);
        BatchSidecar sidecar("tenant-a",store,8,4096,16,8,64);
        sidecar.enqueue(write("a","old"));assert(sidecar.flush()[0].status==Status::Committed);
        store.grant_epoch("tenant-a","a",2);
        sidecar.enqueue(write("a","old",1));assert(sidecar.flush()[0].status==Status::StaleEpoch);
        sidecar.enqueue(write("a","old",2,1));assert(sidecar.flush()[0].status==Status::OperationConflict);
    }
    { // Delimiter/control identities cannot enter tenant, actor, slot or operation keys.
        assert(!valid_id(std::string("bad\0id",6)));
        assert(!valid_id("bad\nactor"));
        assert(!valid_id("bad\tkey"));
        MemoryStore store;
        bool rejected=false;
        try { store.grant_epoch("tenant-a",std::string("bad\0id",6),1); }
        catch (const std::invalid_argument&) { rejected=true; }
        assert(rejected);
    }
    { // Concurrent flush calls must preserve per-actor FIFO and CAS order.
        MemoryStore store;
        BatchSidecar sidecar("tenant-a",store,600,100000,16,1,64);
        for (unsigned i=0; i<200; ++i) {
            const auto actor="concurrent-"+std::to_string(i);
            store.grant_epoch("tenant-a",actor,1);
            sidecar.enqueue(write(actor,"first",1,0));
            sidecar.enqueue(write(actor,"second",1,1));
        }
        std::atomic<bool> go{false};
        std::atomic<unsigned> committed{0}, rejected{0};
        auto consumer=[&] {
            while (!go.load()) std::this_thread::yield();
            while (sidecar.queued_count()) {
                for (const auto& receipt : sidecar.flush()) {
                    if (receipt.status == Status::Committed) ++committed;
                    else ++rejected;
                }
            }
        };
        std::thread first(consumer), second(consumer);
        go.store(true);
        first.join(); second.join();
        assert(committed == 400 && rejected == 0);
        for (unsigned i=0; i<200; ++i)
            assert(store.version({"tenant-a","concurrent-"+std::to_string(i),"main"}) == 2);
    }
    std::cout << "PASS LLVM host-only persistence sidecar contract (8 groups)\n";
}
