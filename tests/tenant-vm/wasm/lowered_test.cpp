// Wasm the attacker controls, lowered to native and run as tenant VMs in one OS process.
// This is the enforcement the addressing rule needs: a validated module has no way to
// express an address outside its own cage.
#include "guest_module.h"
#include "oreslang/runtime/TenantVm.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace oreslang::runtime::vm;
using Bytes = std::vector<std::uint8_t>;

namespace {
int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define REQUIRE_OK(r) do { if ((r).status != Status::Ok) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, to_string((r).status)); ++failures; return 1; } } while (0)

VmSpec spec(const std::string& id, std::uint32_t deadline_ms = 2000) {
    VmSpec s;
    s.tenant_id = id;
    s.module = &ores_vm_lowered_module;
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
std::int64_t word(const Bytes& b) { std::int64_t v = -999999; if (b.size() == 8) std::memcpy(&v, b.data(), 8); return v; }

volatile std::int64_t g_host_secret = 0x0BADC0DE5EC12E7;
} // namespace

int main() {
    TenantVmHost host;
    auto victim = host.open(spec("victim"));
    REQUIRE_OK(victim);
    auto victim_actor = host.spawn_actor(victim.handle, 0);
    REQUIRE_OK(victim_actor);
    constexpr std::int64_t kSecret = 0x5EC12E7;
    CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, kSecret)).get().payload) == kSecret);
    const auto state = word(host.call(victim_actor.actor, msg(OP_STATE_OFFSET)).get().payload);

    // Inside the rule the guest works normally.
    auto peer = host.open(spec("peer"));
    REQUIRE_OK(peer);
    auto peer_actor = host.spawn_actor(peer.handle, 0);
    REQUIRE_OK(peer_actor);
    CHECK(word(host.call(peer_actor.actor, msg(OP_ADD, 5)).get().payload) == 5);
    // The same offset in another VM holds that VM's value, not the victim's.
    CHECK(word(host.call(peer_actor.actor, msg(OP_PEEK, state)).get().payload) == 5);

    // Attack: every address class it can form, including the low and high halves of real
    // host pointers. A successful read must return its own memory. Anything else faults.
    const auto secret_address = reinterpret_cast<std::uintptr_t>(&g_host_secret);
    const auto stack_address = reinterpret_cast<std::uintptr_t>(&failures);
    const std::vector<std::int64_t> addresses = {
        0, 8, 4095, 1 << 20, 0x7FFFFFFF, 0x80000000LL, 0xFFFFFFF8LL, 0xFFFFFFFFLL,
        static_cast<std::int64_t>(secret_address), static_cast<std::int64_t>(secret_address >> 32),
        static_cast<std::int64_t>(stack_address), static_cast<std::int64_t>(stack_address >> 32),
        static_cast<std::int64_t>(secret_address & 0xFFFFFFFF), -1, -8};
    int attempts = 0, stayed_in_cage = 0;
    for (const std::int64_t op : {std::int64_t{OP_PEEK}, std::int64_t{OP_PEEK_FAR}, std::int64_t{OP_POKE}}) {
        for (const std::int64_t address : addresses) {
            auto attacker = host.open(spec("attacker"));
            REQUIRE_OK(attacker);
            auto actor = host.spawn_actor(attacker.handle, 0);
            REQUIRE_OK(actor);
            const auto result = host.call(actor.actor, msg(op, address, 0x41414141)).get();
            ++attempts;
            if (result.status == Status::Ok) {
                CHECK(word(result.payload) != kSecret);
                CHECK(word(result.payload) != g_host_secret);
                ++stayed_in_cage;
                host.kill(attacker.handle);
            }
            const VmTerminal terminal = [&] { if (result.status == Status::Ok) host.kill(attacker.handle); return host.wait_closed(attacker.handle); }();
            if (result.status != Status::Ok) {
                // The proof: a fault is reported with its offset into this VM's own cage.
                // WildAccess would mean the access left the cage.
                if (terminal.reason != FailReason::MemoryFault) {
                    std::fprintf(stderr, "  ESCAPE? op=%lld address=%#llx reason=%s\n", static_cast<long long>(op),
                                 static_cast<unsigned long long>(address), to_string(terminal.reason));
                    ++failures;
                }
                CHECK(terminal.fault_offset < (std::uint64_t{1} << 33));
            }
        }
    }
    std::printf("  %d hostile accesses: %d landed in the attacker's own memory, %d faulted inside its own cage, 0 left it\n",
                attempts, stayed_in_cage, attempts - stayed_in_cage);
    CHECK(g_host_secret == 0x0BADC0DE5EC12E7);
    CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, 0)).get().payload) == kSecret); // untouched

    // Faults and hangs, in lowered code, cost one VM only.
    struct { const char* name; std::int64_t op; std::int64_t a; FailReason want; std::uint32_t deadline; } cases[] = {
        {"unreachable", OP_TRAP, 0, FailReason::Trap, 2000},
        {"spin", OP_SPIN, 0, FailReason::TurnDeadline, 100},
        // clang -O2 may turn this recursion into a loop, so either stop is correct.
        {"recursion", OP_RECURSE, 0, FailReason::StackOverflow, 200},
        {"indirect-call-out-of-range", OP_INDIRECT, 12345, FailReason::Trap, 2000},
        {"indirect-call-null-slot", OP_INDIRECT, 0, FailReason::Trap, 2000},
    };
    std::int64_t victim_value = kSecret;
    for (int round = 0; round < 3; ++round) {
        for (const auto& c : cases) {
            auto v = host.open(spec(c.name, c.deadline));
            REQUIRE_OK(v);
            auto a = host.spawn_actor(v.handle, 0);
            REQUIRE_OK(a);
            CHECK(host.call(a.actor, msg(c.op, c.a)).get().status == Status::TenantFailed);
            const VmTerminal terminal = host.wait_closed(v.handle);
            if (round == 0) std::printf("  %-28s %s\n", c.name, to_string(terminal.reason));
            CHECK(terminal.reason == c.want || (c.op == OP_RECURSE && terminal.reason == FailReason::TurnDeadline));
            victim_value += 1;
            CHECK(word(host.call(victim_actor.actor, msg(OP_ADD, 1)).get().payload) == victim_value);
        }
    }

    // Throughput of lowered code through the whole path.
    auto bench = host.open(spec("bench"));
    REQUIRE_OK(bench);
    auto counter = host.spawn_actor(bench.handle, 0);
    REQUIRE_OK(counter);
    const auto start = std::chrono::steady_clock::now();
    constexpr int kTurns = 200000;
    for (int i = 0; i < kTurns; ++i) host.call(counter.actor, msg(OP_ADD, 1)).get();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    std::printf("  %d sequential calls: %.2f us per round trip\n", kTurns, static_cast<double>(us) / kTurns);

    std::printf("%s\n", failures ? "LOWERED_FAIL" : "LOWERED_PASS");
    return failures != 0;
}
