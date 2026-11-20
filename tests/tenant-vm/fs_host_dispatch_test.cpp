/* Trusted-host-only ABI-v5-shaped filesystem dispatch substrate.
 * Synthetic scheduler witness + real six-layer Linux async FS broker.
 * This deliberately DOES NOT execute a .atto guest or expose a guest ABI. */
#include "oreslang/runtime/TenantVmFsHostDispatch.hpp"
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
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

using oreslang::runtime::vm::internal::FsTurnAuth;
using oreslang::runtime::vm::internal::HostFsDispatch;

static void join(char* dest, std::size_t cap, const char* left, const char* right) {
    const int size = std::snprintf(dest, cap, "%s/%s", left, right);
    assert(size > 0 && static_cast<std::size_t>(size) < cap);
}
static void expect_file(const char* path, const char* expected) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    assert(fd >= 0);
    std::uint8_t output[64] = {};
    const ssize_t read_bytes = read(fd, output, sizeof(output));
    assert(read_bytes == static_cast<ssize_t>(std::strlen(expected)));
    assert(std::memcmp(output, expected, static_cast<std::size_t>(read_bytes)) == 0);
    assert(close(fd) == 0);
}
static void put_file(const char* path, const char* payload) {
    const int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
    assert(fd >= 0);
    const std::size_t length = std::strlen(payload);
    assert(write(fd, payload, length) == static_cast<ssize_t>(length));
    assert(close(fd) == 0);
}
static std::int64_t poll_ready(HostFsDispatch& bridge, const FsTurnAuth& live,
                               std::uint64_t ticket, std::uint32_t output_offset,
                               std::uint32_t capacity) {
    for (unsigned n = 0; n < 1000000u; ++n) {
        const auto result = bridge.poll(live, ticket, output_offset, capacity);
        if (result != ORES_VM_IO_PENDING) return result;
        if ((n & 255u) == 0u) sched_yield();
    }
    return ORES_VM_IO_HOST_ERROR; // test timeout, never silently assert success
}
int main() {
    char base[] = "/tmp/atto-host-fs-dispatch-XXXXXX";
    assert(mkdtemp(base));
    char root[512], data[512], out[512], input[512], copy[512], outside[512];
    join(root, sizeof(root), base, "root");
    join(data, sizeof(data), root, "data");
    join(out, sizeof(out), root, "out");
    join(input, sizeof(input), data, "input.txt");
    join(copy, sizeof(copy), out, "copy.txt");
    join(outside, sizeof(outside), base, "outside.txt");
    assert(mkdir(root, 0700) == 0);
    assert(mkdir(data, 0700) == 0);
    assert(mkdir(out, 0700) == 0);
    put_file(input, "authorized");
    put_file(outside, "outside");
    const atto_fs_layered_policy policy = {
        77u, 51u,
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", nullptr, nullptr},
        {"data", "out", nullptr, nullptr},
        {"*", "*", "data/private", "out/private"}
    };
    atto_fs_broker broker{};
    assert(atto_fs_broker_init_layers(&broker, root, &policy) == 0);
    const atto_fs_host_actor actor{77u, 51u, 0u, 1u}; // slot zero is valid
    atto_fs_async_service* service = nullptr;
    assert(atto_fs_async_create(&service, &broker, &actor, 1u) == 0);
    std::vector<std::uint8_t> cage(131072, 0xBF);
    const FsTurnAuth turn{51u, 0u, 1u, 17u, 4096u, 65536u, 49152u, 16384u};
    HostFsDispatch bridge(turn, 77u, cage.data(), cage.size(), service);
    const auto path_read = std::uint32_t{4100u};
    const auto path_write = std::uint32_t{4150u};
    const auto payload = std::uint32_t{4450u};
    const auto output = std::uint32_t{49152u};
    constexpr std::uint32_t read_length = 14u;  // "data/input.txt"
    constexpr std::uint32_t write_length = 12u; // "out/copy.txt"
    std::memcpy(cage.data() + path_read, "data/input.txt", read_length);
    std::memcpy(cage.data() + path_write, "out/copy.txt", write_length);

    const auto ticket_read = bridge.submit_read(turn, path_read, read_length, 64u);
    assert(ticket_read > 0);
    // A service at its ticket capacity must signal retryability, without
    // minting a false ticket or pretending submit succeeded.
    assert(bridge.submit_read(turn, path_read, read_length, 64u) == ORES_VM_IO_WOULD_BLOCK);
    assert(poll_ready(bridge, turn, static_cast<std::uint64_t>(ticket_read), output, 64u) == 10);
    assert(std::memcmp(cage.data() + output, "authorized", 10u) == 0);
    assert(bridge.poll(turn, static_cast<std::uint64_t>(ticket_read), output, 64u) ==
           ORES_VM_IO_STALE_TICKET);

    std::memcpy(cage.data() + payload, "written", 7u);
    const auto ticket_write = bridge.submit_write(turn, path_write, write_length, payload, 7u);
    assert(ticket_write > 0);
    std::memcpy(cage.data() + payload, "changed", 7u);
    assert(poll_ready(bridge, turn, static_cast<std::uint64_t>(ticket_write), output, 0u) == 0);
    expect_file(copy, "written"); // async service captured guest input before return

    // Invalid spans must be rejected BEFORE touching actor/sibling/guard bytes.
    assert(bridge.submit_read(turn, 4095u, 14u, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_read(turn, UINT32_MAX, 14u, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_read(turn, path_read, 0u, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_read(turn, path_read, 1024u, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_read(turn, path_read, read_length, 0u) == ORES_VM_IO_TOO_LARGE);
    assert(bridge.submit_read(turn, path_read, read_length, 1048577u) == ORES_VM_IO_TOO_LARGE);
    assert(bridge.submit_write(turn, path_write, write_length, 4095u, 7u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_write(turn, path_write, write_length, UINT32_MAX, 7u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_write(turn, path_write, write_length, payload, 1048577u) ==
           ORES_VM_IO_TOO_LARGE);
    assert(bridge.poll(turn, 1u, 49151u, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.poll(turn, 1u, UINT32_MAX, 64u) == ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.poll(turn, 1u, output, 1048577u) == ORES_VM_IO_TOO_LARGE);
    assert(bridge.poll(turn, 0u, output, 64u) == ORES_VM_IO_STALE_TICKET);
    assert(bridge.cancel(turn, 0u) == ORES_VM_IO_STALE_TICKET);
    cage[path_read + 4] = 0;
    assert(bridge.submit_read(turn, path_read, read_length, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    cage[path_read + 4] = '/';

    auto sibling = turn;
    sibling.actor_ref = 1u;
    auto stale = turn;
    stale.actor_incarnation = 2u; // unchanged actor_ref
    auto later_turn = turn;
    later_turn.turn_sequence = 19u;
    auto wrong_tenant = turn;
    wrong_tenant.tenant_generation = 52u;
    for (const auto& unauthorized : {sibling, stale, later_turn, wrong_tenant}) {
        assert(bridge.submit_read(unauthorized, path_read, read_length, 64u) ==
               ORES_VM_IO_PERMISSION_DENIED);
        assert(bridge.submit_write(unauthorized, path_write, write_length, payload, 7u) ==
               ORES_VM_IO_PERMISSION_DENIED);
        assert(bridge.poll(unauthorized, 1u, output, 64u) == ORES_VM_IO_PERMISSION_DENIED);
        assert(bridge.cancel(unauthorized, 1u) == ORES_VM_IO_PERMISSION_DENIED);
    }
    HostFsDispatch bad_owner(turn, 78u, cage.data(), cage.size(), service);
    assert(bad_owner.submit_read(turn, path_read, read_length, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    HostFsDispatch small_cage(turn, 77u, cage.data(), 1024u, service);
    assert(small_cage.submit_read(turn, path_read, read_length, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);

    // Six-layer denies cannot be bypassed by passing a different path.
    const auto denied_write = bridge.submit_write(turn, path_read, read_length, payload, 7u);
    assert(denied_write > 0);
    assert(poll_ready(bridge, turn, static_cast<std::uint64_t>(denied_write), output, 0u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    expect_file(input, "authorized");

    const char* escape = "../outside.txt";
    std::memcpy(cage.data() + path_read, escape, std::strlen(escape));
    const auto traversal = bridge.submit_read(turn, path_read,
                                               static_cast<std::uint32_t>(std::strlen(escape)), 64u);
    assert(traversal > 0);
    assert(poll_ready(bridge, turn, static_cast<std::uint64_t>(traversal), output, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    expect_file(outside, "outside");

    // Lifecycle cut-off denies all new operations and all ticket probes.
    std::memcpy(cage.data() + path_read, "data/input.txt", read_length);
    const auto inflight = bridge.submit_read(turn, path_read, read_length, 64u);
    assert(inflight > 0);
    assert(atto_fs_async_revoke_actor(service, &actor) == 0);
    const auto before = cage[output];
    assert(bridge.poll(turn, static_cast<std::uint64_t>(inflight), output, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);
    assert(cage[output] == before);
    assert(bridge.cancel(turn, static_cast<std::uint64_t>(inflight)) ==
           ORES_VM_IO_PERMISSION_DENIED);
    assert(bridge.submit_read(turn, path_read, read_length, 64u) ==
           ORES_VM_IO_PERMISSION_DENIED);

    assert(atto_fs_async_forget_actor(service, &actor) == 0);
    atto_fs_async_destroy(service); // trusted lifecycle worker joins I/O
    atto_fs_broker_close(&broker);
    assert(unlink(input) == 0 && unlink(copy) == 0 && unlink(outside) == 0);
    assert(rmdir(data) == 0 && rmdir(out) == 0 && rmdir(root) == 0 && rmdir(base) == 0);
    std::puts("ATTO_VM_FS_HOST_DISPATCH_REAL_BROKER_HOST_ONLY_PASS");
}
