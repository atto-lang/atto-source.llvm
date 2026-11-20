// HOST-ONLY: actual TenantVM scheduler lifecycle + six-layer Linux broker.
// Does not compile a .atto guest or activate guest filesystem ABI v5.
#include "oreslang/runtime/TenantVm.hpp"
#include "atto/host_fs_async.h"
#include "guests.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using namespace oreslang::runtime::vm;

namespace {
atto_fs_host_actor c_owner(const HostFsActorIdentity& id) noexcept {
    return {id.tenant_id, id.tenant_generation, id.actor_ref, id.actor_incarnation};
}
bool authenticated_owner(void* service, const HostFsActorIdentity& id) noexcept {
    const auto actor = c_owner(id);
    return atto_fs_async_owner_matches(static_cast<atto_fs_async_service*>(service), &actor) == 1;
}
bool revoke_owner(void* service, const HostFsActorIdentity& id) noexcept {
    const auto actor = c_owner(id);
    return atto_fs_async_revoke_actor(static_cast<atto_fs_async_service*>(service), &actor) == 0;
}
HostFsLifecycleBinding binding(std::uint64_t tenant, atto_fs_async_service* service) {
    return {tenant, service, &authenticated_owner, &revoke_owner};
}
std::vector<std::uint8_t> words(std::int64_t a, std::int64_t b = 0) {
    std::vector<std::uint8_t> bytes(16);
    std::memcpy(bytes.data(), &a, sizeof(a));
    std::memcpy(bytes.data() + 8, &b, sizeof(b));
    return bytes;
}
VmSpec spec(const char* name) {
    VmSpec out;
    out.tenant_id = name;
    out.module = &ores_vm_test_module;
    out.limits.restart_backoff_ms = 1;
    out.limits.turn_deadline_ms = 2000;
    return out;
}
void path_join(char* out, std::size_t len, const char* a, const char* b) {
    const int n = std::snprintf(out, len, "%s/%s", a, b);
    assert(n > 0 && static_cast<std::size_t>(n) < len);
}
void make_file(const char* p, const char* s) {
    const int fd = open(p, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    assert(write(fd, s, std::strlen(s)) == static_cast<ssize_t>(std::strlen(s)));
    assert(close(fd) == 0);
}
void expect_file(const char* p, const char* text) {
    const int fd = open(p, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    char value[64] = {};
    const ssize_t read_size = read(fd, value, sizeof(value));
    assert(read_size == static_cast<ssize_t>(std::strlen(text)));
    assert(std::memcmp(value, text, static_cast<std::size_t>(read_size)) == 0);
    assert(close(fd) == 0);
}
int await_ticket(atto_fs_async_service* service, const atto_fs_host_actor& actor,
                 std::uint64_t ticket, std::uint8_t* output,
                 std::size_t capacity, std::size_t* bytes) {
    for (unsigned i = 0; i < 2000000u; ++i) {
        const int rc = atto_fs_async_poll(service, &actor, ticket, output, capacity, bytes);
        if (rc != -EAGAIN) return rc;
        if ((i & 511u) == 0u) sched_yield();
    }
    return -ETIMEDOUT;
}
} // namespace

int main() {
    char base[] = "/tmp/atto-fs-vm-turn-XXXXXX";
    assert(mkdtemp(base) != nullptr);
    char root[512], data[512], out[512], input[512], copy[512];
    path_join(root, sizeof(root), base, "root");
    path_join(data, sizeof(data), root, "data");
    path_join(out, sizeof(out), root, "out");
    path_join(input, sizeof(input), data, "input.txt");
    path_join(copy, sizeof(copy), out, "copy.txt");
    assert(mkdir(root, 0700) == 0 && mkdir(data, 0700) == 0 && mkdir(out, 0700) == 0);
    make_file(input, "authorized");

    TenantVmHost host;
    auto opened = host.open(spec("actor-fs-lifecycle"));
    assert(opened.status == Status::Ok);
    const auto actor = host.spawn_actor(opened.handle, KIND_RESTARTABLE);
    const auto sibling = host.spawn_actor(opened.handle, KIND_COUNTER);
    assert(actor.status == Status::Ok && sibling.status == Status::Ok);
    // Actor-ref zero is a legitimate scheduler slot, not an invalid identity.
    assert(actor.actor.ref() == 0u);

    constexpr std::uint64_t tenant = 73u;
    const atto_fs_layered_policy policy = {
        tenant, opened.handle.generation(),
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", "data/private", "out/private"}
    };
    atto_fs_broker broker{};
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    const atto_fs_host_actor owner{tenant, opened.handle.generation(), actor.actor.ref(), 1u};
    const atto_fs_host_actor wrong{tenant, opened.handle.generation(), sibling.actor.ref(), 1u};
    atto_fs_async_service* old = nullptr;
    assert(atto_fs_async_create(&old, &broker, &owner, 4u) == 0);
    assert(host.bind_actor_fs_lifecycle(sibling.actor, binding(tenant, old)) == Status::InvalidArgument);
    assert(host.bind_actor_fs_lifecycle(actor.actor, binding(tenant, old)) == Status::Ok);
    assert(host.bind_actor_fs_lifecycle(actor.actor, binding(tenant, old)) == Status::Busy);

    // Check real six-layer broker read/write under the service owner, before
    // the actor's restart revokes the service permanently.
    std::uint64_t ticket = 0;
    std::uint8_t output[64] = {};
    std::size_t size = 0;
    assert(atto_fs_async_submit_read(old, &owner, "data/input.txt", sizeof(output), &ticket) == 0);
    assert(await_ticket(old, owner, ticket, output, sizeof(output), &size) == 0);
    assert(size == 10u && std::memcmp(output, "authorized", size) == 0);
    assert(atto_fs_async_submit_write(old, &owner, "out/copy.txt", output, size, &ticket) == 0);
    assert(await_ticket(old, owner, ticket, nullptr, 0, &size) == 0);
    expect_file(copy, "authorized");

    // Live actor restart preserves actor_ref but increments FS incarnation.
    // Its old service and outstanding tickets can NEVER be used again.
    assert(atto_fs_async_submit_read(old, &owner, "data/input.txt", 64u, &ticket) == 0);
    const auto restarted = host.call(actor.actor, words(RESTART_ADD_THEN_TRAP, 1)).get();
    assert(restarted.status == Status::ActorRestarted);
    assert(atto_fs_async_owner_matches(old, &owner) == 0);
    std::uint64_t denied = 42u;
    assert(atto_fs_async_submit_write(old, &owner, "out/copy.txt",
                                      reinterpret_cast<const std::uint8_t*>("corrupt"), 7u,
                                      &denied) == -EACCES);
    assert(denied == 0u);
    std::memset(output, 0xA5, sizeof(output));
    assert(atto_fs_async_poll(old, &owner, ticket, output, sizeof(output), &size) == -EACCES);
    assert(output[0] == 0xA5 && size == 0u);
    assert(atto_fs_async_cancel(old, &owner, ticket) == -EACCES);
    expect_file(copy, "authorized");

    // Rebind only with the new incarnation; sibling and cross-TenantVM
    // identical actor-ref values remain ineligible.
    const atto_fs_host_actor reincarnated{tenant, opened.handle.generation(),
                                           actor.actor.ref(), 2u};
    atto_fs_async_service* fresh = nullptr;
    assert(atto_fs_async_create(&fresh, &broker, &reincarnated, 4u) == 0);
    assert(host.bind_actor_fs_lifecycle(actor.actor, binding(tenant, old)) == Status::InvalidArgument);
    assert(host.bind_actor_fs_lifecycle(actor.actor, binding(tenant, fresh)) == Status::Ok);
    const auto another = host.open(spec("actor-fs-foreign-vm"));
    assert(another.status == Status::Ok);
    const auto foreign = host.spawn_actor(another.handle, KIND_COUNTER);
    assert(foreign.status == Status::Ok);
    assert(foreign.actor.ref() == actor.actor.ref());
    assert(host.bind_actor_fs_lifecycle(foreign.actor, binding(tenant, fresh)) == Status::InvalidArgument);

    // Stopping an actor revokes immediately, before draining its mailbox.
    assert(host.stop_actor(actor.actor) == Status::Ok);
    denied = 17u;
    assert(atto_fs_async_submit_read(fresh, &reincarnated, "data/input.txt", 16u, &denied) == -EACCES);
    assert(denied == 0u);

    // Quiesce and kill both revoke an independently bound actor.
    const auto on_shutdown = host.spawn_actor(opened.handle, KIND_COUNTER);
    assert(on_shutdown.status == Status::Ok);
    const atto_fs_host_actor shut_owner{tenant, opened.handle.generation(),
                                        on_shutdown.actor.ref(), 1u};
    atto_fs_async_service* shut_service = nullptr;
    assert(atto_fs_async_create(&shut_service, &broker, &shut_owner, 2u) == 0);
    assert(host.bind_actor_fs_lifecycle(on_shutdown.actor, binding(tenant, shut_service)) == Status::Ok);
    assert(host.begin_shutdown(opened.handle) == Status::Ok);
    assert(atto_fs_async_owner_matches(shut_service, &shut_owner) == 0);
    assert(host.wait_closed(opened.handle).phase == Phase::Stopped);

    // Foreign VM has the same numeric actor ref but a different host VM
    // generation. Killing it must revoke its independent service.
    const atto_fs_layered_policy foreign_policy = {
        74u, another.handle.generation(),
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", nullptr, nullptr}
    };
    atto_fs_broker foreign_broker{};
    assert(atto_fs_broker_init_layers(&foreign_broker, root, &foreign_policy) == 0);
    const atto_fs_host_actor foreign_owner{74u, another.handle.generation(), foreign.actor.ref(), 1u};
    atto_fs_async_service* foreign_service = nullptr;
    assert(atto_fs_async_create(&foreign_service, &foreign_broker, &foreign_owner, 2u) == 0);
    assert(host.bind_actor_fs_lifecycle(foreign.actor, binding(74u, foreign_service)) == Status::Ok);
    assert(host.kill(another.handle) == Status::Ok);
    assert(atto_fs_async_owner_matches(foreign_service, &foreign_owner) == 0);
    assert(host.wait_closed(another.handle).phase == Phase::Failed);

    // All carrier threads are quiescent and have dropped borrowed bindings
    // before blocking cleanup. No service destruction happens on carriers.
    assert(atto_fs_async_forget_actor(old, &owner) == 0);
    assert(atto_fs_async_forget_actor(fresh, &reincarnated) == 0);
    assert(atto_fs_async_forget_actor(shut_service, &shut_owner) == 0);
    assert(atto_fs_async_forget_actor(foreign_service, &foreign_owner) == 0);
    atto_fs_async_destroy(old);
    atto_fs_async_destroy(fresh);
    atto_fs_async_destroy(shut_service);
    atto_fs_async_destroy(foreign_service);
    atto_fs_broker_close(&broker);
    atto_fs_broker_close(&foreign_broker);

    assert(unlink(input) == 0 && unlink(copy) == 0);
    assert(rmdir(data) == 0 && rmdir(out) == 0 && rmdir(root) == 0 && rmdir(base) == 0);
    std::puts("ATTO_FS_SCHEDULER_TO_BROKER_LIFECYCLE_HOST_ONLY_PASS");
}
