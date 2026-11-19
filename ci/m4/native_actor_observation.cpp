// Cross-backend M4: compile the exact shared .atto source and observe native guest behavior.
// This is NOT a production hostile-native-code sandbox certification.
#include "oreslang/runtime/TenantVm.hpp"
#include "oreslang/runtime/TenantVmGuestLoader.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace oreslang::runtime::vm;
using Bytes = std::vector<std::uint8_t>;

static void require(bool good, const char* what) {
    if (!good) throw std::runtime_error(what);
}
static std::int64_t word(const Bytes& b) {
    require(b.size() == 8, "expected exactly one i64 result");
    std::int64_t value = 0;
    std::memcpy(&value, b.data(), sizeof(value));
    return value;
}
static Bytes message(std::int64_t n) {
    Bytes b(sizeof(n));
    std::memcpy(b.data(), &n, sizeof(n));
    return b;
}
static VmSpec spec(const std::string& id, const LoadedGuest& guest) {
    VmSpec s;
    s.tenant_id = id;
    s.module = guest.module;
    s.module_owner = guest.owner;
    s.limits.turn_deadline_ms = 3000;
    return s;
}
static std::int64_t send(TenantVmHost& host, const VmActor& actor, std::int64_t n) {
    const auto response = host.call(actor, message(n)).get();
    require(response.status == Status::Ok, "real guest actor call failed");
    return word(response.payload);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: m4_native_guest <guest.so> <sha256>\n");
        return 2;
    }
    try {
        const LoadedGuest guest = load_guest_library(argv[1], argv[2]);
        require(guest.error.empty() && guest.module != nullptr, "native guest failed exact-digest admission");
        require(guest.module->kind_count == 2, "expected Counter actor + native main kinds");

        TenantVmHost host;
        auto observed = host.open(spec("m4-observed-calls", guest));
        require(observed.status == Status::Ok, "cannot open isolated observation tenant");
        auto first = host.spawn_actor(observed.handle, 0);
        auto second = host.spawn_actor(observed.handle, 0);
        require(first.status == Status::Ok && second.status == Status::Ok,
                "failed to create two independently stored actors");

        std::int64_t digest = 0;
        const std::int64_t first_post_1 = send(host, first.actor, 7);
        digest = digest * 31 + first_post_1;
        const std::int64_t second_post_1 = send(host, second.actor, 9);
        digest = digest * 31 + second_post_1;
        const std::int64_t first_post_2 = send(host, first.actor, 8);
        digest = digest * 31 + first_post_2;
        require(first_post_1 == 7 && second_post_1 == 9 && first_post_2 == 15,
                "LLVM actor state differs from the pinned M4 transcript");
        require(digest == 7021, "LLVM actor reply rolling digest differs from GraalVM");
        require(host.stop_actor(first.actor) == Status::Ok, "cannot retire first actor");
        require(host.stop_actor(second.actor) == Status::Ok, "cannot retire second actor");
        require(host.begin_shutdown(observed.handle) == Status::Ok, "cannot stop observation tenant");
        const auto observed_result = host.wait_closed(observed.handle);
        require(observed_result.phase == Phase::Stopped, "observation tenant never stopped");
        require(observed_result.actor_restarts == 0, "guest actor restarted unexpectedly");

        // This is a distinct, real execution of the SAME guest program's compiled
        // main entry; the prior per-message trace is host-visible actor reply
        // telemetry, NOT an assertion that main itself emitted a digest.
        auto program = host.open(spec("m4-compiled-source-main", guest));
        require(program.status == Status::Ok, "cannot open source-main tenant");
        auto main_actor = host.spawn_actor(program.handle, 1);
        require(main_actor.status == Status::Ok, "cannot spawn source-compiled main actor");
        const auto result = host.call(main_actor.actor, {}).get();
        require(result.status == Status::Ok, "source-compiled main did not execute");
        require(word(result.payload) == 43, "source-compiled main result differs from GraalVM");
        require(host.begin_shutdown(program.handle) == Status::Ok, "cannot shut down main tenant");
        const auto terminal = host.wait_closed(program.handle);
        require(terminal.phase == Phase::Stopped, "source-main tenant failed to drain");
        require(terminal.actor_restarts == 0, "source-main worker unexpectedly restarted");
        require(terminal.mail_lost == 0, "source-main mailbox lost an admitted message");

        std::puts("ATTO_M4_LLVM_SOURCE_GUEST_RETURN=43");
        std::puts("ATTO_M4_LLVM_ACTOR_CALLS_OBSERVED=3");
        std::puts("ATTO_M4_LLVM_OBSERVATION_ACTOR_STOPS=2");
        std::puts("ATTO_M4_LLVM_HOST_OBSERVED_ACTOR_DIGEST=7021");
        std::puts("ATTO_M4_LLVM_MAIN_GUEST_DRAINED=1");
        std::puts("ATTO_M4_SCOPE=ACTUAL_LLVM_TENANT_GUEST_NOT_PRODUCTION_SANDBOX");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ATTO_M4_LLVM_GUEST_FAILED: %s\n", e.what());
        return 1;
    }
}
