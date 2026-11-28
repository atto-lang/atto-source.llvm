// Two OS processes over a local socket, plus explicit EOF/unknown handling.
// Test-only trusted peer fixture; this is NOT an authenticated production wire.
#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef __linux__
#include <cerrno>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace oreslang::runtime;

static IpcReceiptEnvelope message(std::uint64_t seq, std::string session) {
    IpcReceiptEnvelope e;
    e.domain="tenant"; e.sender="producer"; e.destination="worker";
    e.session=std::move(session); e.payload_sha256=std::string(64, 'a');
    e.sender_incarnation=7; e.destination_incarnation=9; e.route_epoch=4;
    e.capability_generation=2; e.message_sequence=seq;
    e.frame_sequence=seq;
    return e;
}

#ifdef __linux__
enum class ChildCase { crash_before_ack, crash_after_ack, completed, forged_ack };
static void run_case(ChildCase scenario) {
    // A freshly connected AF_UNIX stream supplies actual Linux SO_PEERCRED
    // identity; socketpair created before fork does not bind the child PID.
    int listener = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(listener >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    address.sun_path[0] = '\0';  // Linux abstract namespace, no path/symlink.
    const int length = std::snprintf(address.sun_path + 1,
            sizeof(address.sun_path) - 1, "oreslang-receipt-%ld",
            static_cast<long>(getpid()));
    assert(length > 0 && static_cast<std::size_t>(length) < sizeof(address.sun_path) - 1);
    const socklen_t address_size = static_cast<socklen_t>(
            offsetof(sockaddr_un, sun_path) + 1 + length);
    assert(bind(listener, reinterpret_cast<const sockaddr*>(&address), address_size) == 0);
    assert(listen(listener, 1) == 0);
    NativeIpcReceiptTable table(2);
    const auto original = message(1, "sessionA");
    table.admit(original);
    table.commit_dispatch(original);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(listener);
        const int client = socket(AF_UNIX, SOCK_STREAM, 0);
        if (client < 0 || connect(client,
                reinterpret_cast<const sockaddr*>(&address), address_size) != 0)
            _exit(90);
        const char* bytes = scenario == ChildCase::crash_before_ack ? "" :
            scenario == ChildCase::crash_after_ack ? "A" :
            scenario == ChildCase::completed ? "AD" : "X";
        const std::size_t count = std::char_traits<char>::length(bytes);
        if (count > 0 && send(client, bytes, count, MSG_NOSIGNAL)
                != static_cast<ssize_t>(count))
            _exit(91);
        close(client);
        _exit(0);
    }
    const int connection = accept(listener, nullptr, nullptr);
    assert(connection >= 0);
    close(listener);
    struct PeerCredentials { pid_t pid; uid_t uid; gid_t gid; } credentials{};
    socklen_t cred_size = sizeof(credentials);
    assert(getsockopt(connection, SOL_SOCKET, SO_PEERCRED,
                      &credentials, &cred_size) == 0);
    assert(cred_size == sizeof(credentials));
    assert(credentials.pid == pid && credentials.uid == geteuid()
           && credentials.gid == getegid());
    struct timeval timeout = {3, 0};
    assert(setsockopt(connection, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0);
    bool ack = false, done = false, forged_rejected = false;
    char step = 0;
    for (;;) {
        ssize_t n = recv(connection, &step, 1, 0);
        if (n == 0) break;
        assert(n == 1);
        if (step == 'A') {
            table.accept_verified(original, IpcReceiptStage::remote_accepted);
            ack = true;
        } else if (step == 'D') {
            table.accept_verified(original, IpcReceiptStage::handler_committed);
            done = true;
        } else {
            assert(step == 'X');
            auto forged = original;
            forged.session = "sessionB";
            try {
                table.accept_verified(forged, IpcReceiptStage::remote_accepted);
            } catch (const std::invalid_argument&) {
                forged_rejected = true;
            }
        }
        assert(table.invariant());
    }
    close(connection);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const std::size_t uncertain = table.mark_transport_lost(original);
    assert(uncertain == (done ? 0u : 1u));
    assert(table.mark_transport_lost(original) == 0);
    assert(table.counters() == (std::pair<std::size_t, std::size_t>{0,0}));
    assert(table.invariant());
    if (scenario == ChildCase::crash_before_ack) assert(!ack && !done);
    if (scenario == ChildCase::crash_after_ack) assert(ack && !done);
    if (scenario == ChildCase::completed) assert(ack && done);
    if (scenario == ChildCase::forged_ack) assert(forged_rejected && !ack);
    try { table.admit(original); assert(false && "replay permitted"); }
    catch (const std::invalid_argument&) { }
}
#endif

int main() {
#ifdef __linux__
    run_case(ChildCase::crash_before_ack);
    run_case(ChildCase::crash_after_ack);
    run_case(ChildCase::completed);
    run_case(ChildCase::forged_ack);
    // A delayed EOF from an old route/grant must not retire a replacement
    // connection that reuses the same textual session name.
    NativeIpcReceiptTable cross_epoch(3);
    auto old_message = message(1, "sessionA");
    auto new_message = message(2, "sessionA");
    ++new_message.route_epoch;
    ++new_message.capability_generation;
    cross_epoch.admit(old_message);
    cross_epoch.admit(new_message);
    assert(cross_epoch.mark_transport_lost(old_message) == 1);
    assert(cross_epoch.counters() == (std::pair<std::size_t, std::size_t>{1,1}));
    cross_epoch.commit_dispatch(new_message);
    cross_epoch.accept_verified(new_message, IpcReceiptStage::remote_accepted);
    cross_epoch.accept_verified(new_message, IpcReceiptStage::handler_committed);
    assert(cross_epoch.counters() == (std::pair<std::size_t, std::size_t>{0,0}));
    assert(cross_epoch.invariant());
    std::cout << "native-ipc-two-process-EOF PASS (NOT authenticated transport)\n";
#else
    std::cout << "native-ipc-two-process: unsupported target (Linux required)\n";
#endif
}
