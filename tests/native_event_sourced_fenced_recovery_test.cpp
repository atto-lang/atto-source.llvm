// Native host integration only: real SQLite WAL/FULL journal, fork/_exit crash
// between provider commit and caller ACK, provider-fenced replay, fresh arena.
// Does NOT compile or execute .atto source nor implement guest actor dispatch.
#include "oreslang/runtime/ActorOwnedHeap.hpp"
#include "oreslang/runtime/SqliteBatchPersistence.hpp"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace oreslang::runtime::persistence;
using oreslang::runtime::ActorHeapIdentity;
using oreslang::runtime::ActorOwnedHeap;
using Heap = ActorOwnedHeap<256, 4>;

static std::vector<std::uint8_t> encode(std::uint64_t value) {
    std::vector<std::uint8_t> data(8);
    for (unsigned i = 0; i < 8; ++i)
        data[i] = static_cast<std::uint8_t>(value >> (56 - 8 * i));
    return data;
}
static std::uint64_t decode(const std::vector<std::uint8_t>& data) {
    if (data.size() != 8) throw std::invalid_argument("INVALID_EVENT_SCHEMA");
    std::uint64_t value = 0;
    for (auto byte : data) value = (value << 8) | byte;
    return value;
}
static Write deposit(const Identity& identity, std::uint64_t epoch) {
    return {identity, "deposit-op-42", epoch, 0, 1, WriteKind::Event, encode(42)};
}

int main() {
    char template_path[] = "/tmp/atto-native-event-XXXXXX";
    char* folder = mkdtemp(template_path);
    if (!folder) throw std::runtime_error("cannot create temporary persistence directory");
    const std::string db = std::string(folder) + "/journal.sqlite";
    const Identity account{"tenant-a", "bank-42", "main"};

    // This is an actual child process boundary; the process exits immediately
    // after storage commits without executing C++ destructors, publishing the
    // new in-memory state or sending the original command reply.
    pid_t pid = fork();
    if (pid < 0) throw std::runtime_error("fork failed");
    if (pid == 0) {
        try {
            SqliteStore store(db);
            store.grant_epoch(account.tenant, account.actor, 1);
            Heap first(ActorHeapIdentity{42, 7, 1});
            auto memory = first.allocate(sizeof(std::uint64_t), alignof(std::uint64_t));
            if (!memory || !first.write(*memory, std::uint64_t{0})) _exit(11);
            BatchSidecar sidecar(account.tenant, store, 16, 4096, 64, 16, 256);
            sidecar.enqueue(deposit(account, 1));
            auto receipts = sidecar.flush();
            if (receipts.size() != 1 || receipts.front().status != Status::Committed ||
                !receipts.front().durable() || receipts.front().version != 1) _exit(12);
            auto visible = first.read<std::uint64_t>(*memory);
            if (!visible || *visible != 0) _exit(13);
            // A volatile success reply is not sent. Simulated abrupt process death.
            _exit(0);
        } catch (...) {
            _exit(14);
        }
    }
    int child_status = 0;
    assert(waitpid(pid, &child_status, 0) == pid);
    assert(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0);

    {
        SqliteStore store(db);
        store.grant_epoch(account.tenant, account.actor, 2);
        bool stale_replay = false;
        try { (void)store.load_fenced(account, 1); }
        catch (const std::logic_error&) { stale_replay = true; }
        assert(stale_replay && "old incarnation unexpectedly loaded authoritative state");

        const auto restoration = store.load_fenced(account, 2);
        assert(restoration.version == 1 && restoration.snapshot_version == 0 &&
               restoration.snapshot.empty() && restoration.events.size() == 1);
        std::uint64_t balance = 0;
        std::uint64_t expected = 0;
        for (const auto& event : restoration.events) {
            assert(event.version == expected + 1 && event.schema_version == 1);
            balance += decode(event.payload);
            expected = event.version;
        }
        assert(balance == 42 && expected == restoration.version);

        Heap second(ActorHeapIdentity{42, 7, 2});
        auto new_state = second.allocate(sizeof(std::uint64_t), alignof(std::uint64_t));
        assert(new_state && second.write(*new_state, balance));
        auto published = second.read<std::uint64_t>(*new_state);
        assert(published && *published == 42);

        // A new epoch can read-only verify a prior committed EVENT without
        // reacquiring an old writer's lease or appending another journal row.
        auto proof=store.resolve_committed_event_fenced(
            account,2,"deposit-op-42",1,encode(42));
        assert(proof && proof->version==1 && proof->schema_version==1 &&
               proof->payload==encode(42));
        assert(!store.resolve_committed_event_fenced(
            account,2,"missing-op",1,encode(42)).has_value());
        bool bad_payload=false;
        try { (void)store.resolve_committed_event_fenced(
            account,2,"deposit-op-42",1,encode(43)); }
        catch (const std::logic_error&) { bad_payload=true; }
        assert(bad_payload && "mismatched event bytes accepted");
        bool stale_lookup=false;
        try { (void)store.resolve_committed_event_fenced(
            account,1,"deposit-op-42",1,encode(42)); }
        catch (const std::logic_error&) { stale_lookup=true; }
        assert(stale_lookup && "stale actor inspected a committed event");

        // Existing providers prevent cross-epoch duplicate appends but cannot
        // return the prior durable command response to the new activation.
        // Resolution by stable command ID is the next P0 feature, not proven here.
        auto conflict = store.commit({deposit(account, 2)});
        assert(conflict.size() == 1 &&
               conflict[0].status == Status::OperationConflict &&
               !conflict[0].durable());
        assert(store.load_fenced(account, 2).events.size() == 1);
        auto old_writer = store.commit({deposit(account, 1)});
        assert(old_writer.size() == 1 && old_writer[0].status == Status::StaleEpoch);
        second.retire_quiesced();
        assert(!second.read<std::uint64_t>(*new_state));
    }
    std::filesystem::remove_all(folder);
    std::cout << "PASS native HOST event journal crash-after-commit before ACK; fenced replay\n";
    return 0;
}
