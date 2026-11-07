// Lowered guests, loaded as a real service would load them, run as tenant VMs in one process.
//   hostile_*:  Wasm the attacker controls, in both memory modes
//   atto_*:     real AttoLang source, compiled by oreslang-llvmc and lowered
#include "hostile_ops.h"
#include "oreslang/runtime/TenantVm.hpp"
#include "oreslang/runtime/TenantVmGuestLoader.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace oreslang::runtime::vm;
using Bytes = std::vector<std::uint8_t>;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define CHECK_STATUS(actual, want) do { const Status va = (actual); if (va != (want)) { std::fprintf(stderr, "  FAIL %s:%d: got %s, want %s\n", __FILE__, __LINE__, to_string(va), to_string(want)); ++failures; } } while (0)
#define CHECK_REASON(actual, want) do { const FailReason va = (actual); if (va != (want)) { std::fprintf(stderr, "  FAIL %s:%d: got %s, want %s\n", __FILE__, __LINE__, to_string(va), to_string(want)); ++failures; } } while (0)
#define REQUIRE_OK(r) do { if ((r).status != Status::Ok) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, to_string((r).status)); ++failures; return; } } while (0)

std::string g_root;

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream out;
    out << in.rdbuf();
    return out.str();
}
std::string library_path(const std::string& name) {
#if defined(__APPLE__)
    return g_root + "/" + name + "/guest.dylib";
#else
    return g_root + "/" + name + "/guest.so";
#endif
}
std::string digest_of(const std::string& name) {
    const std::string manifest = read_file(g_root + "/" + name + "/manifest.json");
    const auto at = manifest.find("\"sha256\": \"");
    if (at == std::string::npos) return "";
    return manifest.substr(at + 11, 64);
}
LoadedGuest load(const std::string& name) {
    LoadedGuest guest = load_guest_library(library_path(name), digest_of(name));
    if (!guest.error.empty()) {
        std::fprintf(stderr, "  FAIL loading %s: %s\n", name.c_str(), guest.error.c_str());
        ++failures;
    }
    return guest;
}
VmSpec spec(const std::string& id, const LoadedGuest& guest, std::uint32_t deadline_ms = 2000) {
    VmSpec s;
    s.tenant_id = id;
    s.module = guest.module;
    s.module_owner = guest.owner;
    s.limits.turn_deadline_ms = deadline_ms;
    return s;
}
Bytes msg(std::int64_t op, std::int64_t a = 0, std::int64_t b = 0) {
    Bytes out(24);
    std::memcpy(out.data(), &op, 8);
    std::memcpy(out.data() + 8, &a, 8);
    std::memcpy(out.data() + 16, &b, 8);
    return out;
}
[[maybe_unused]] Bytes one(std::int64_t v) {
    Bytes out(8);
    std::memcpy(out.data(), &v, 8);
    return out;
}
std::int64_t word(const Bytes& b) {
    std::int64_t v = -999999;
    if (b.size() == 8) std::memcpy(&v, b.data(), 8);
    return v;
}
volatile std::int64_t g_host_secret = 0x0BADC0DE5EC12E7;

// ---------------------------------------------------------------- hostile Wasm

// Every address class Wasm can express, including the halves of real host pointers.
void attack_matrix(const char* library, bool bounded) {
    const LoadedGuest guest = load(library);
    if (!guest.module) return;
    TenantVmHost host;
    constexpr std::int64_t kSecret = 0x5EC12E7;
    auto victim = host.open(spec("victim", guest));
    REQUIRE_OK(victim);
    auto victim_actor = host.spawn_actor(victim.handle, 0);
    REQUIRE_OK(victim_actor);
    CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, kSecret)).get().payload) == kSecret);

    const auto secret_address = reinterpret_cast<std::uintptr_t>(&g_host_secret);
    const auto stack_address = reinterpret_cast<std::uintptr_t>(&failures);
    const std::vector<std::int64_t> addresses = {
        0, 8, 4095, 1 << 16, 1 << 20, 0x7FFFFFFF, 0x80000000LL, 0xFFFFFFF8LL, 0xFFFFFFFFLL,
        static_cast<std::int64_t>(secret_address), static_cast<std::int64_t>(secret_address >> 32),
        static_cast<std::int64_t>(stack_address), static_cast<std::int64_t>(stack_address >> 32),
        static_cast<std::int64_t>(secret_address & 0xFFFFFFFF), -1, -8};
    int attempts = 0, stayed = 0, faulted = 0;
    const struct { std::int64_t op; std::int64_t b; } attacks[] = {
        {OP_PEEK, 0}, {OP_PEEK_FAR, 0}, {OP_POKE, 0x41414141}, {OP_POKE, -1 /* largest displacement */}};
    for (const auto& attack : attacks) {
        for (const std::int64_t address : addresses) {
            auto attacker = host.open(spec("attacker", guest));
            REQUIRE_OK(attacker);
            auto actor = host.spawn_actor(attacker.handle, 0);
            REQUIRE_OK(actor);
            const auto result = host.call(actor.actor, msg(attack.op, address, attack.b)).get();
            ++attempts;
            if (result.status == Status::Ok) {
                CHECK(word(result.payload) != kSecret);
                CHECK(word(result.payload) != g_host_secret);
                ++stayed;
                host.kill(attacker.handle);
            } else {
                ++faulted;
            }
            const VmTerminal terminal = host.wait_closed(attacker.handle);
            if (result.status != Status::Ok) {
                // The proof. Bounds mode stops it in generated code. Guard mode has no such
                // check, so the access really happens, and the fault must be in its own cage.
                const FailReason expected = bounded ? FailReason::Trap : FailReason::MemoryFault;
                if (terminal.reason != expected) {
                    std::fprintf(stderr, "  ESCAPE? op=%lld address=%#llx reason=%s\n", static_cast<long long>(attack.op),
                                 static_cast<unsigned long long>(address), to_string(terminal.reason));
                    ++failures;
                }
            }
        }
    }
    std::printf("  %s: %d hostile accesses: %d in its own memory, %d stopped, 0 left the cage\n", library, attempts, stayed, faulted);
    CHECK(host.stats().wild_accesses == 0);
    CHECK(g_host_secret == 0x0BADC0DE5EC12E7);
    CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, 0)).get().payload) == kSecret);

    // memory.grow is refused, not silently honoured past what the region holds.
    auto probe = host.open(spec("probe", guest));
    REQUIRE_OK(probe);
    auto probe_actor = host.spawn_actor(probe.handle, 0);
    CHECK(word(host.call(probe_actor.actor, msg(OP_GROW)).get().payload) == -1);

    // Host calls with references it made up. Each is refused, and a refusal ends only this VM.
    for (const std::int64_t bad : {std::int64_t{-1}, std::int64_t{1} << 40, std::int64_t{999},
                                   static_cast<std::int64_t>(secret_address)}) {
        for (const std::int64_t op : {std::int64_t{OP_VM_SEND}, std::int64_t{OP_VM_STOP}, std::int64_t{OP_VM_SPAWN}}) {
            auto v = host.open(spec("forger", guest));
            REQUIRE_OK(v);
            auto a = host.spawn_actor(v.handle, 0);
            REQUIRE_OK(a);
            CHECK_STATUS(host.call(a.actor, msg(op, bad, 1)).get().status, Status::TenantFailed);
            CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
        }
    }
    {
        // References from another VM mean nothing here: the slot resolves to this VM's own actor,
        // or to nothing. So a real spawn in one, used in another, cannot touch the first.
        auto one = host.open(spec("one", guest));
        auto two = host.open(spec("two", guest));
        REQUIRE_OK(one);
        REQUIRE_OK(two);
        auto a1 = host.spawn_actor(one.handle, 0);
        auto a2 = host.spawn_actor(two.handle, 0);
        const auto reference = word(host.call(a1.actor, msg(OP_VM_SPAWN, 0)).get().payload);
        CHECK(reference != 0);
        CHECK_STATUS(host.call(a2.actor, msg(OP_VM_STOP, reference)).get().status, Status::TenantFailed);
        CHECK_REASON(host.wait_closed(two.handle).reason, FailReason::Trap);
        // And it still works where it belongs.
        const auto live = host.snapshot(one.handle).live_actors;
        CHECK(live == 2);
        host.call(a1.actor, msg(OP_VM_STOP, reference)).get();
        CHECK(host.snapshot(one.handle).live_actors == 1);
        host.kill(one.handle);
        host.wait_closed(one.handle);
    }

    // Faults and hangs in lowered code cost one VM only.
    struct { const char* name; std::int64_t op; std::int64_t a; FailReason want; std::uint32_t deadline; } cases[] = {
        {"unreachable", OP_TRAP, 0, FailReason::Trap, 2000},
        {"spin", OP_SPIN, 0, FailReason::TurnDeadline, 100},
        {"recursion", OP_RECURSE, 0, FailReason::StackOverflow, 300}, // clang may make this a loop
        {"indirect-call-out-of-range", OP_INDIRECT, 12345, FailReason::Trap, 2000},
        {"indirect-call-null-slot", OP_INDIRECT, 0, FailReason::Trap, 2000},
    };
    std::int64_t expected = kSecret;
    for (int round = 0; round < 3; ++round) {
        for (const auto& c : cases) {
            auto v = host.open(spec(c.name, guest, c.deadline));
            REQUIRE_OK(v);
            auto a = host.spawn_actor(v.handle, 0);
            REQUIRE_OK(a);
            CHECK_STATUS(host.call(a.actor, msg(c.op, c.a)).get().status, Status::TenantFailed);
            const VmTerminal terminal = host.wait_closed(v.handle);
            if (round == 0) std::printf("  %-28s %s\n", c.name, to_string(terminal.reason));
            CHECK(terminal.reason == c.want || (c.op == OP_RECURSE && terminal.reason == FailReason::TurnDeadline));
            expected += 1;
            CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, 1)).get().payload) == expected);
        }
    }
}

// A hostile module can recurse until a host call is what crosses the end of the stack. The host
// is not in guest mode at that point, so without a check on entry the fault ends the process.
void overflow_in_a_host_call_costs_one_vm() {
    const LoadedGuest guest = load("hostile_bounds");
    if (!guest.module) return;
    HostLimits limits;
    limits.carriers = 2;
    limits.carrier_stack_bytes = std::size_t{1} << 20; // fewer levels, each paying for a spawn
    TenantVmHost host(limits);
    auto witness = host.open(spec("witness", guest));
    REQUIRE_OK(witness);
    auto witness_actor = host.spawn_actor(witness.handle, 0);
    REQUIRE_OK(witness_actor);
    for (int round = 0; round < 3; ++round) {
        auto v = host.open(spec("recursing", guest, 20000));
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, 0);
        REQUIRE_OK(a);
        CHECK_STATUS(host.call(a.actor, msg(OP_RECURSE_IN_HOST_CALLS, 0)).get().status, Status::TenantFailed);
        CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::StackOverflow);
        CHECK(word(host.call(witness_actor.actor, msg(OP_ADD, 1)).get().payload) == round + 1);
    }
}

// Bounds checks also keep actors of one tenant apart. Without them they share a cage, which
// is not a boundary this design promises, and is the reason bounds are the default.
void actors_of_one_tenant() {
    const LoadedGuest bounded = load("hostile_bounds");
    if (!bounded.module) return;
    TenantVmHost host;
    auto v = host.open(spec("t", bounded));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, 0);
    auto b = host.spawn_actor(v.handle, 0);
    REQUIRE_OK(a);
    REQUIRE_OK(b);
    CHECK(word(host.call(a.actor, msg(OP_ADD, 11)).get().payload) == 11);
    CHECK(word(host.call(b.actor, msg(OP_ADD, 22)).get().payload) == 22);
    CHECK(word(host.call(a.actor, msg(OP_ADD, 0)).get().payload) == 11); // memory is per actor
    // The same address names a different actor's own memory, and past it Wasm traps. The region
    // holds 128 KiB of memory and 4 KiB of scratch, so this is past a's memory by 1 MiB.
    auto r = host.call(b.actor, msg(OP_PEEK, 1 << 20)).get();
    CHECK_STATUS(r.status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
}

// -------------------------------------------------------------- the loader

void loader_admits_only_exact_bytes() {
    // FIPS 180 test vectors.
    CHECK(sha256_hex("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string longer(1000, 'a');
    CHECK(sha256_hex(longer.data(), longer.size()) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");

    const std::string path = library_path("hostile_bounds"), digest = digest_of("hostile_bounds");
    CHECK(load_guest_library(path, digest).module != nullptr);
    CHECK(load_guest_library(path, std::string(64, '0')).module == nullptr); // wrong digest
    CHECK(load_guest_library(path, "").module == nullptr);
    CHECK(load_guest_library(path + ".missing", digest).module == nullptr);
    CHECK(load_guest_library(g_root, digest).module == nullptr); // a directory

    // One changed byte in a copy, so the digest no longer matches.
    const std::string copy = g_root + "/tampered.lib";
    std::string bytes = read_file(path);
    bytes[bytes.size() / 2] = static_cast<char>(bytes[bytes.size() / 2] ^ 1);
    { std::ofstream out(copy, std::ios::binary | std::ios::trunc); out << bytes; }
    CHECK(load_guest_library(copy, digest).module == nullptr);
    // A symlink to the right bytes is refused too: names are not what is being admitted.
    const std::string link = g_root + "/link.lib";
    unlink(link.c_str());
    if (symlink(path.c_str(), link.c_str()) == 0) CHECK(load_guest_library(link, digest).module == nullptr);
    unlink(link.c_str());
    unlink(copy.c_str());

    // Libraries loaded one after another, each gone before the next, are each their own code.
    // The first version of this loader on Linux returned the first library again and again. The
    // two builds tell themselves apart by how they stop an out-of-range read.
    for (int round = 0; round < 6; ++round) {
        const bool bounded = round % 2 == 0;
        TenantVmHost reloading;
        LoadedGuest guest = load(bounded ? "hostile_bounds" : "hostile_guard");
        auto v = reloading.open(spec("reload", guest));
        REQUIRE_OK(v);
        auto a = reloading.spawn_actor(v.handle, 0);
        REQUIRE_OK(a);
        CHECK_STATUS(reloading.call(a.actor, msg(OP_PEEK, 0x7FFFFFF0LL)).get().status, Status::TenantFailed);
        CHECK_REASON(reloading.wait_closed(v.handle).reason, bounded ? FailReason::Trap : FailReason::MemoryFault);
    }

    // The library stays mapped for as long as a VM needs it, not as long as the caller holds it.
    TenantVmHost host;
    VmHandle handle;
    VmActor actor;
    {
        LoadedGuest guest = load("hostile_bounds");
        auto v = host.open(spec("kept", guest));
        REQUIRE_OK(v);
        handle = v.handle;
        auto a = host.spawn_actor(v.handle, 0);
        REQUIRE_OK(a);
        actor = a.actor;
    }
    CHECK(word(host.call(actor, msg(OP_ADD, 7)).get().payload) == 7);
    host.kill(handle);
    host.wait_closed(handle);
    // And loading repeatedly in one process leaves nothing behind that stops it working.
    for (int i = 0; i < 200; ++i) {
        LoadedGuest guest = load("hostile_bounds");
        auto v = host.open(spec("churn", guest));
        REQUIRE_OK(v);
        auto a = host.spawn_actor(v.handle, 0);
        CHECK(word(host.call(a.actor, msg(OP_ADD, i)).get().payload) == i);
        host.kill(v.handle);
        host.wait_closed(v.handle);
    }
}

// ------------------------------------------------------------- real source

#if defined(TENANT_VM_HAVE_ATTO)
void atto_actor_state_is_private_and_durable() {
    const LoadedGuest counter = load("atto_counter");
    const LoadedGuest doubler = load("atto_doubler");
    if (!counter.module || !doubler.module) return;
    TenantVmHost host;
    auto acme = host.open(spec("acme", counter));
    auto globex = host.open(spec("globex", counter));
    auto initech = host.open(spec("initech", doubler)); // other code, same export names, same process
    REQUIRE_OK(acme);
    REQUIRE_OK(globex);
    REQUIRE_OK(initech);
    auto a1 = host.spawn_actor(acme.handle, 0);
    auto a2 = host.spawn_actor(acme.handle, 0);
    auto g1 = host.spawn_actor(globex.handle, 0);
    auto i1 = host.spawn_actor(initech.handle, 0);
    REQUIRE_OK(a1);
    REQUIRE_OK(a2);
    REQUIRE_OK(g1);
    REQUIRE_OK(i1);
    std::int64_t expected = 0;
    for (int i = 1; i <= 1000; ++i) {
        expected += i;
        CHECK(word(host.call(a1.actor, one(i)).get().payload) == expected); // survives every turn
    }
    CHECK(word(host.call(a2.actor, one(5)).get().payload) == 5);            // another actor
    CHECK(word(host.call(g1.actor, one(7)).get().payload) == 7);            // another tenant
    CHECK(word(host.call(i1.actor, one(0)).get().payload) == 2);            // another program
    CHECK(word(host.call(i1.actor, one(0)).get().payload) == 4);
    CHECK(word(host.call(a1.actor, one(0)).get().payload) == expected);     // nobody touched it
    // A wrong message shape is the guest's typed error, not a fault.
    auto wrong = host.call(a1.actor, Bytes(3)).get();
    CHECK_STATUS(wrong.status, Status::GuestError);
    // A stopped actor's replacement starts from its initializer, not what the last one left.
    CHECK_STATUS(host.stop_actor(a2.actor), Status::Ok);
    auto a3 = host.spawn_actor(acme.handle, 0);
    REQUIRE_OK(a3);
    CHECK(word(host.call(a3.actor, one(1)).get().payload) == 1);
    std::printf("  1000 durable turns; state private per actor, tenant and program\n");
}

// A trap in source restarts that actor from its last finished turn. Everything else carries on.
void atto_failed_actor_restarts_from_stored_state() {
    const LoadedGuest counter = load("atto_counter");
    if (!counter.module) return;
    CHECK(counter.module->kinds[0].flags == ORES_VM_KIND_RESTARTABLE);
    TenantVmHost host;
    VmSpec s = spec("t", counter);
    s.limits.max_parallel_turns = 1;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, 0);
    auto sibling = host.spawn_actor(v.handle, 0);
    REQUIRE_OK(a);
    REQUIRE_OK(sibling);
    const auto max = std::numeric_limits<std::int64_t>::max();
    CHECK(word(host.call(sibling.actor, one(3)).get().payload) == 3);
    CHECK(word(host.call(a.actor, one(max)).get().payload) == max);
    auto overflow = host.call(a.actor, one(1));        // checked add traps
    auto queued = host.call(a.actor, one(-max));       // waiting behind it, on the same actor
    const auto failed = overflow.get();
    CHECK_STATUS(failed.status, Status::ActorRestarted);
    CHECK_REASON(failed.failure, FailReason::Trap);
    CHECK(word(queued.get().payload) == 0);            // mail carried over, against the restored total
    CHECK(word(host.call(a.actor, one(5)).get().payload) == 5);
    CHECK(word(host.call(sibling.actor, one(1)).get().payload) == 4);
    CHECK(host.snapshot(v.handle).actor_restarts == 1);
    CHECK(host.stats().wild_accesses == 0);
}

// The guard build has no bounds checks, so nothing shows a fault was confined: it ends the VM.
void atto_without_bounds_checks_no_restart() {
    TenantVmHost host;
    const LoadedGuest guard = load("hostile_guard");
    if (!guard.module) return;
    CHECK(guard.module->kinds[0].flags == 0);
    auto v = host.open(spec("t", guard));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, 0);
    REQUIRE_OK(a);
    CHECK_STATUS(host.call(a.actor, msg(OP_TRAP)).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
}

void atto_runaway_source_ends_only_its_turn() {
    const LoadedGuest spin = load("atto_spin");
    const LoadedGuest recurse = load("atto_recurse");
    const LoadedGuest counter = load("atto_counter");
    if (!spin.module || !recurse.module || !counter.module) return;
    TenantVmHost host;
    auto bystander = host.open(spec("bystander", counter));
    REQUIRE_OK(bystander);
    auto b = host.spawn_actor(bystander.handle, 0);
    auto s = host.open(spec("spinner", spin, 100));
    auto r = host.open(spec("recursive", recurse, 500));
    REQUIRE_OK(s);
    REQUIRE_OK(r);
    auto sa = host.spawn_actor(s.handle, 0);
    auto ra = host.spawn_actor(r.handle, 0);
    std::int64_t expected = 0;
    for (int round = 0; round < 3; ++round) { // the budget is 3 restarts in a window
        const auto spun = host.call(sa.actor, one(1)).get();
        CHECK_STATUS(spun.status, Status::ActorRestarted);
        const auto recursed = host.call(ra.actor, one(1)).get();
        CHECK_STATUS(recursed.status, Status::ActorRestarted);
        if (round == 0) std::printf("  source spin: %s, source recursion: %s\n", to_string(spun.failure), to_string(recursed.failure));
        expected += 1;
        CHECK(word(host.call(b.actor, one(1)).get().payload) == expected);
    }
    // That spent the budget. The next failure ends that actor, and its VM goes on.
    // Which reason depends on the toolchain: clang may compile this recursion to a loop or to a trap.
    const auto ended = host.call(sa.actor, one(1)).get();
    CHECK_STATUS(ended.status, Status::ActorTerminated);
    CHECK(ended.failure == FailReason::Trap || ended.failure == FailReason::TurnDeadline);
    CHECK_STATUS(host.call(sa.actor, one(1)).get().status, Status::ActorTerminated);
    const VmSnapshot snapshot = host.snapshot(s.handle);
    CHECK(snapshot.phase == Phase::Running);
    CHECK(snapshot.actor_restarts == 3);
    CHECK(snapshot.actors_terminated == 1);
    CHECK(host.snapshot(r.handle).phase == Phase::Running);
    CHECK(word(host.call(b.actor, one(1)).get().payload) == expected + 1);
    CHECK(host.stats().wild_accesses == 0);
}

// A fault exactly in the middle of a commit, after one of two fields is written. Then the same thing in a
// build without the flip that makes a commit atomic, which must tear, or this test cannot tell.
void atto_a_fault_in_a_commit_cannot_tear_state() {
    const auto run = [](const char* library) {
        const LoadedGuest pair = load(library);
        TenantVmHost host;
        auto v = host.open(spec("pair", pair));
        auto a = host.spawn_actor(v.handle, 0);
        CHECK(word(host.call(a.actor, one(10)).get().payload) == 0);
        CHECK(word(host.call(a.actor, one(10)).get().payload) == 0);
        CHECK_STATUS(host.call(a.actor, one(77)).get().status, Status::ActorRestarted); // faults in its commit
        return word(host.call(a.actor, one(1)).get().payload);
    };
    CHECK(run("atto_pair_fault") == 0);
    const std::int64_t torn = run("atto_pair_unsafe");
    CHECK(torn != 0);
    std::printf("  a fault mid-commit leaves the fields consistent; without the flip they differ by %lld\n", static_cast<long long>(torn));
}

// Kills land at random points in many turns. Two fields that must always move together show whether a
// restart ever gets half of a commit. The window is narrow, so the case above is the one with teeth.
void atto_kills_never_tear_stored_state() {
    const LoadedGuest pair = load("atto_pair");
    if (!pair.module) return;
    TenantVmHost host;
    VmSpec s = spec("pair", pair, 1);
    s.limits.restart_budget = 65535;
    s.limits.restart_backoff_ms = 0;
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, 0);
    REQUIRE_OK(a);
    std::uint64_t seed = 0xFEED;
    const auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    int completed = 0, killed = 0;
    // Find turn lengths around the deadline, so that a turn is about as likely to end as to be killed.
    std::int64_t length = 1000;
    for (int i = 0; i < 4000 && (killed < 300 || completed < 300); ++i) {
        const auto r = host.call(a.actor, one(static_cast<std::int64_t>(static_cast<double>(length) * (0.5 + static_cast<double>(next() % 100) / 100.0)))).get();
        if (r.status == Status::Ok) {
            CHECK(word(r.payload) == 0); // first minus second, after whatever was restored
            ++completed;
            if (i % 8 == 0) length += length / 4;
        } else {
            CHECK_STATUS(r.status, Status::ActorRestarted);
            CHECK_REASON(r.failure, FailReason::TurnDeadline);
            ++killed;
            if (i % 8 == 0 && length > 100) length -= length / 8;
        }
    }
    std::printf("  %d turns completed and %d killed part way, with state never torn\n", completed, killed);
    CHECK(completed > 0);
    CHECK(killed > 0);
    CHECK(host.snapshot(v.handle).phase == Phase::Running);
}

void atto_helper_functions_run_in_a_turn() {
    const LoadedGuest work = load("atto_work");
    if (!work.module) return;
    TenantVmHost host;
    auto v = host.open(spec("w", work));
    REQUIRE_OK(v);
    auto a = host.spawn_actor(v.handle, 0);
    REQUIRE_OK(a);
    CHECK(word(host.call(a.actor, one(10)).get().payload) == 55);
    CHECK(word(host.call(a.actor, one(1000)).get().payload) == 500500);
}

// A real program: main spawns workers of two types, sends to them, and stops them.
// Ordered traps unless it sees 1, 2, 3... in order, so a reordered or lost message ends the VM.
void run_main(TenantVmHost& host, const VmHandle& vm, std::uint32_t main_kind, std::int64_t expected) {
    auto entry = host.spawn_actor(vm, main_kind);
    REQUIRE_OK(entry);
    auto r = host.call(entry.actor, {}).get();
    CHECK_STATUS(r.status, Status::Ok);
    CHECK(word(r.payload) == expected);
    CHECK_STATUS(host.call(entry.actor, {}).get().status, Status::GuestError); // main runs once
}

void atto_main_spawns_sends_and_stops() {
    const LoadedGuest fleet = load("atto_fleet");
    if (!fleet.module) return;
    CHECK(fleet.module->kind_count == 3); // Ordered, Summer, main
    TenantVmHost host;
    for (int round = 0; round < 200; ++round) {
        auto v = host.open(spec("fleet", fleet));
        REQUIRE_OK(v);
        run_main(host, v.handle, 2, 43);
        CHECK_STATUS(host.begin_shutdown(v.handle), Status::Ok);
        const VmTerminal terminal = host.wait_closed(v.handle); // drains every worker first
        CHECK(terminal.phase == Phase::Stopped);
        CHECK_REASON(terminal.reason, FailReason::None);
        CHECK(terminal.actor_restarts == 0); // no worker saw mail out of order
    }
    // Everything ran: 1 main + 10 + 10 + 3 worker turns. All three workers ended and were released.
    auto v = host.open(spec("counted", fleet));
    REQUIRE_OK(v);
    run_main(host, v.handle, 2, 43);
    for (int i = 0; i < 2000; ++i) {
        const auto s = host.snapshot(v.handle);
        if (s.live_actors == 1 && s.inflight == 0) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto s = host.snapshot(v.handle);
    CHECK(s.turns == 24 + 1); // the second call to main is a turn as well
    CHECK(s.live_actors == 1);
    CHECK(s.status == Status::Ok);
    std::printf("  main spawned 3 workers of 2 types, 23 sends in order, all stopped and released\n");
}

// Proves the check above has teeth: the same worker, sent 2 first, fails. The worker restarts, the
// program finishes, and the VM's final report still says that a message was lost.
void atto_misordered_mail_is_detected() {
    const LoadedGuest misordered = load("atto_misordered");
    if (!misordered.module) return;
    TenantVmHost host;
    auto v = host.open(spec("misordered", misordered));
    REQUIRE_OK(v);
    auto entry = host.spawn_actor(v.handle, 1);
    REQUIRE_OK(entry);
    CHECK(word(host.call(entry.actor, {}).get().payload) == 43);
    CHECK_STATUS(host.begin_shutdown(v.handle), Status::Ok);
    const VmTerminal terminal = host.wait_closed(v.handle);
    CHECK(terminal.phase == Phase::Stopped);
    CHECK(terminal.actor_restarts == 1);
    CHECK(terminal.mail_lost == 1);
}

// A full mailbox cannot lose a message quietly: the VM ends and says so.
void atto_refused_send_ends_the_vm() {
    const LoadedGuest fleet = load("atto_fleet");
    if (!fleet.module) return;
    TenantVmHost host;
    VmSpec s = spec("tiny-mailbox", fleet);
    s.limits.mailbox_capacity = 1;
    s.limits.max_parallel_turns = 1; // nothing drains while main runs
    auto v = host.open(s);
    REQUIRE_OK(v);
    auto entry = host.spawn_actor(v.handle, 2);
    REQUIRE_OK(entry);
    CHECK_STATUS(host.call(entry.actor, {}).get().status, Status::TenantFailed);
    CHECK_REASON(host.wait_closed(v.handle).reason, FailReason::Trap);
}

void atto_thousand_tenants_run_programs() {
    const LoadedGuest fleet = load("atto_fleet");
    if (!fleet.module) return;
    TenantVmHost host;
    constexpr int kVms = 1000;
    std::vector<VmHandle> vms;
    std::vector<std::future<TurnResult>> results;
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kVms; ++i) {
        VmSpec s = spec("program-" + std::to_string(i), fleet);
        s.limits.heap_bytes = std::uint64_t{8} << 20;
        auto v = host.open(s);
        REQUIRE_OK(v);
        vms.push_back(v.handle);
        auto entry = host.spawn_actor(v.handle, 2);
        REQUIRE_OK(entry);
        results.push_back(host.call(entry.actor, {}));
    }
    for (auto& r : results) CHECK(word(r.get().payload) == 43);
    for (auto& v : vms) CHECK_STATUS(host.begin_shutdown(v), Status::Ok);
    for (auto& v : vms) {
        const VmTerminal terminal = host.wait_closed(v);
        CHECK(terminal.phase == Phase::Stopped);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %d tenants each ran a spawn, send and stop program to completion, one process: %lld ms\n", kVms,
                static_cast<long long>(ms));
    CHECK(host.stats().wild_accesses == 0);
}

// The stated requirement, with real compiled source instead of test guests.
void atto_thousand_tenants_in_one_process() {
    const LoadedGuest counter = load("atto_counter");
    if (!counter.module) return;
    HostLimits limits;
    limits.carriers = 8;
    TenantVmHost host(limits);
    constexpr int kVms = 1000, kActors = 100, kRounds = 3;
    struct Tenant { VmHandle handle; std::vector<VmActor> actors; };
    std::vector<Tenant> tenants(kVms);
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kVms; ++i) {
        VmSpec s = spec("tenant-" + std::to_string(i), counter);
        s.limits.heap_bytes = std::uint64_t{32} << 20;
        auto v = host.open(s);
        REQUIRE_OK(v);
        tenants[i].handle = v.handle;
        for (int j = 0; j < kActors; ++j) {
            auto a = host.spawn_actor(v.handle, 0);
            REQUIRE_OK(a);
            tenants[i].actors.push_back(a.actor);
        }
    }
    // A distinct value per actor, so any crossed wire shows as a wrong total.
    for (int round = 1; round <= kRounds; ++round) {
        std::vector<std::future<TurnResult>> pending;
        pending.reserve(kVms * kActors);
        for (int i = 0; i < kVms; ++i)
            for (int j = 0; j < kActors; ++j) pending.push_back(host.call(tenants[i].actors[j], one(i * 1000 + j)));
        std::size_t at = 0;
        for (int i = 0; i < kVms; ++i)
            for (int j = 0; j < kActors; ++j) {
                const auto r = pending[at++].get();
                CHECK_STATUS(r.status, Status::Ok);
                CHECK(word(r.payload) == (i * 1000 + j) * round);
            }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %d tenants, %d compiled .atto actors, %d turns, one process: %lld ms\n", kVms, kVms * kActors, kVms * kActors * kRounds,
                static_cast<long long>(ms));
    CHECK(host.stats().live_vms == kVms);
}
#endif

// Runs whatever the pipeline accepted, with hostile inputs. Used by the mutation fuzz, where this
// process dying, or a fault outside the cage, is the failure. A guest ending its VM is fine.
int run_accepted_guest(const std::string& name) {
    const LoadedGuest guest = load(name);
    if (!guest.module) return 3;
    TenantVmHost host;
    std::uint64_t seed = 0xC0FFEE;
    const auto next = [&seed] {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        return seed;
    };
    auto bystander = host.open(spec("bystander", load("hostile_bounds")));
    if (bystander.status != Status::Ok) return 4;
    auto witness = host.spawn_actor(bystander.handle, 0);
    std::int64_t expected = 0;
    for (int i = 0; i < 40; ++i) {
        auto v = host.open(spec("mutant", guest, 50));
        if (v.status != Status::Ok) return 5;
        auto a = host.spawn_actor(v.handle, 0);
        if (a.status != Status::Ok) return 6;
        for (int turn = 0; turn < 4; ++turn) {
            auto r = host.call(a.actor, msg(static_cast<std::int64_t>(next() % 13), static_cast<std::int64_t>(next() >> (next() % 64)),
                                            static_cast<std::int64_t>(next()))).get();
            if (r.status == Status::TenantFailed || r.status == Status::NotRunning) break;
        }
        host.kill(v.handle);
        host.wait_closed(v.handle);
        expected += 1;
        if (word(host.call(witness.actor, msg(OP_ADD, 1)).get().payload) != expected) return 7;
    }
    if (host.stats().wild_accesses != 0) {
        std::fprintf(stderr, "a guest faulted outside its cage\n");
        return 8;
    }
    return 0;
}

struct Case { const char* name; void (*run)(); };
const Case cases[] = {
    {"overflow_in_a_host_call_costs_one_vm", overflow_in_a_host_call_costs_one_vm},
    {"attack_matrix_bounds", [] { attack_matrix("hostile_bounds", true); }},
    {"attack_matrix_guard_pages_alone", [] { attack_matrix("hostile_guard", false); }},
    {"actors_of_one_tenant", actors_of_one_tenant},
    {"loader_admits_only_exact_bytes", loader_admits_only_exact_bytes},
#if defined(TENANT_VM_HAVE_ATTO)
    {"atto_actor_state_is_private_and_durable", atto_actor_state_is_private_and_durable},
    {"atto_failed_actor_restarts_from_stored_state", atto_failed_actor_restarts_from_stored_state},
    {"atto_without_bounds_checks_no_restart", atto_without_bounds_checks_no_restart},
    {"atto_runaway_source_ends_only_its_turn", atto_runaway_source_ends_only_its_turn},
    {"atto_a_fault_in_a_commit_cannot_tear_state", atto_a_fault_in_a_commit_cannot_tear_state},
    {"atto_kills_never_tear_stored_state", atto_kills_never_tear_stored_state},
    {"atto_helper_functions_run_in_a_turn", atto_helper_functions_run_in_a_turn},
    {"atto_thousand_tenants_in_one_process", atto_thousand_tenants_in_one_process},
    {"atto_main_spawns_sends_and_stops", atto_main_spawns_sends_and_stops},
    {"atto_misordered_mail_is_detected", atto_misordered_mail_is_detected},
    {"atto_refused_send_ends_the_vm", atto_refused_send_ends_the_vm},
    {"atto_thousand_tenants_run_programs", atto_thousand_tenants_run_programs},
#endif
};
} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: tenant_vm_lowered_test <lowered-dir> [case]\n");
        return 2;
    }
    g_root = argv[1];
    if (argc == 4 && std::strcmp(argv[2], "--run-accepted") == 0) return run_accepted_guest(argv[3]);
    int ran = 0, failed = 0;
    for (const Case& c : cases) {
        if (argc > 2 && std::strcmp(argv[2], c.name) != 0) continue;
        std::printf("[ RUN  ] %s\n", c.name);
        std::fflush(stdout);
        const int before = failures;
        c.run();
        const bool pass = failures == before;
        std::printf("[ %s ] %s\n", pass ? " OK " : "FAIL", c.name);
        std::fflush(stdout);
        ++ran;
        if (!pass) ++failed;
    }
#if !defined(TENANT_VM_HAVE_ATTO)
    std::printf("NOT_EXECUTED: .atto cases, this build has no compiler or IR\n");
#endif
    std::printf("%d cases, %d failed\n", ran, failed);
    return ran == 0 || failed != 0;
}
