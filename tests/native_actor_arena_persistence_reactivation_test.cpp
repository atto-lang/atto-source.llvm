// Native actor-owned arena + real SQLite durable storage integration.
// This tests authentic Oreslang RUNTIME memory ownership, not source .ores lowering.
#include "oreslang/runtime/ActorOwnedHeap.hpp"
#include "oreslang/runtime/SqliteBatchPersistence.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <unistd.h> // mkdtemp on macOS
#include <filesystem>
#include <iostream>
#include <string>

using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedHeap;
using namespace oreslang::runtime::persistence;
using Heap = ActorOwnedHeap<256, 4>;

static std::vector<std::uint8_t> encode_u64(std::uint64_t value) {
    std::vector<std::uint8_t> bytes(8);
    // Explicit big endian: Java's ByteBuffer default; no host pointer/address
    // or native Handle is ever persisted.
    for (unsigned i=0; i<8; ++i) bytes[i] = static_cast<std::uint8_t>(value >> (56-8*i));
    return bytes;
}
static std::uint64_t decode_u64(const std::vector<std::uint8_t>& bytes) {
    if(bytes.size()!=8)throw std::invalid_argument("invalid persisted schema");
    std::uint64_t result=0;
    for(auto b:bytes)result=(result<<8)|b;
    return result;
}
static Write snapshot(const Identity& id, std::uint64_t epoch,
    std::uint64_t expected_version, std::string operation, std::uint64_t amount) {
    return {id,std::move(operation),epoch,expected_version,1,
        WriteKind::Snapshot,encode_u64(amount)};
}
int main() {
    char name[]="/tmp/ores-actor-arena-persistence-XXXXXX";
    char* dir=mkdtemp(name);
    if(!dir)throw std::runtime_error("unable to create temporary private database");
    const std::string db=std::string(dir)+"/state.sqlite";
    const Identity stable{"tenant-a","ledger-42","balance"};
    using Handle = Heap::Handle;
    Handle stale;
    {
        SqliteStore store(db);
        store.grant_epoch(stable.tenant,stable.actor,1);
        Heap first(ActorHeapIdentity{42,7,1});
        auto owned=first.allocate(sizeof(std::uint64_t), alignof(std::uint64_t));
        assert(owned.has_value());
        stale=*owned;
        assert(first.write(*owned,std::uint64_t{1}));
        auto initial=first.read<std::uint64_t>(*owned);assert(initial && *initial==1);
        assert(first.write(*owned,std::uint64_t{42}));
        auto state=first.read<std::uint64_t>(*owned);
        assert(state && *state==42);
        BatchSidecar writer("tenant-a",store,16,4096,64,16,256);
        writer.enqueue(snapshot(stable,1,0,"first-deposit",*state));
        auto commit=writer.flush();
        assert(commit.size()==1 && commit[0].status==Status::Committed && commit[0].durable());
        // Passivation makes every old arena Handle invalid even if the next
        // actor reuses the same logical actor key.
        first.retire_quiesced();
        assert(first.retired() && !first.read<std::uint64_t>(*owned));
    }
    {
        SqliteStore reopened(db);
        reopened.grant_epoch(stable.tenant,stable.actor,2);
        bool stale_reactivation_denied=false;
        try { (void)reopened.load_fenced(stable,1); }
        catch(const std::logic_error&) { stale_reactivation_denied=true; }
        assert(stale_reactivation_denied);
        auto restored=reopened.load_fenced(stable,2);
        assert(restored.version==1 && restored.snapshot_version==1 && restored.snapshot_schema==1);
        assert(restored.events.empty());
        const auto payload=decode_u64(restored.snapshot);
        assert(payload==42);
        Heap second(ActorHeapIdentity{42,7,2});
        auto fresh=second.allocate(sizeof(std::uint64_t),alignof(std::uint64_t));
        assert(fresh.has_value() && !second.read<std::uint64_t>(stale));
        assert(second.write(*fresh,payload));
        auto recovered=second.read<std::uint64_t>(*fresh);
        assert(recovered && *recovered==42);
        Heap otherTenantActor(ActorHeapIdentity{42,8,2});
        assert(!otherTenantActor.read<std::uint64_t>(*fresh));
        // Directory activation fencing must also be enforced DURING storage
        // commit. Retired code cannot use its old epoch to replay a receipt.
        auto rejected=reopened.commit({snapshot(stable,1,0,"first-deposit",42)});
        assert(rejected.size()==1 && rejected[0].status==Status::StaleEpoch &&
            !rejected[0].durable());
        second.retire_quiesced();
        assert(!second.read<std::uint64_t>(*fresh));
    }
    std::filesystem::remove_all(dir);
    std::cout<<"PASS native actor-owned arena -> durable SQLite -> retire -> new activation restore\n";
}
