// Two OS processes over a local socket, plus explicit EOF/unknown handling.
// Test-only trusted peer fixture; this is NOT an authenticated production wire.
#include "oreslang/runtime/NativeIpcReceiptTable.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#ifdef __linux__
#include <cerrno>
#include <sys/socket.h>
#include <sys/types.h>
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
    int sockets[2] = {-1,-1};
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    NativeIpcReceiptTable table(2);
    const auto original = message(1, "sessionA");
    table.admit(original);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(sockets[0]);
        const char* bytes = scenario == ChildCase::crash_before_ack ? "" :
            scenario == ChildCase::crash_after_ack ? "A" :
            scenario == ChildCase::completed ? "AD" : "X";
        const std::size_t count = std::char_traits<char>::length(bytes);
        if (count > 0 && send(sockets[1], bytes, count, MSG_NOSIGNAL)
                != static_cast<ssize_t>(count))
            _exit(91);
        close(sockets[1]);
        _exit(0);
    }
    close(sockets[1]);
    struct timeval timeout = {3, 0};
    assert(setsockopt(sockets[0], SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout)) == 0);
    bool ack = false, done = false, forged_rejected = false;
    char step = 0;
    for (;;) {
        ssize_t n = recv(sockets[0], &step, 1, 0);
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
    close(sockets[0]);
    int status = 0;
    assert(waitpid(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    const std::size_t uncertain = table.mark_transport_lost("sessionA");
    assert(uncertain == (done ? 0u : 1u));
    assert(table.mark_transport_lost("sessionA") == 0);
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
    NativeIpcReceiptTable cross_session(3);
    auto old_message = message(1, "sessionA");
    auto new_message = message(2, "sessionB");
    ++new_message.route_epoch;
    cross_session.admit(old_message);
    cross_session.admit(new_message);
    assert(cross_session.mark_transport_lost("sessionA") == 1);
    assert(cross_session.counters() == (std::pair<std::size_t, std::size_t>{1,1}));
    cross_session.accept_verified(new_message, IpcReceiptStage::remote_accepted);
    cross_session.accept_verified(new_message, IpcReceiptStage::handler_committed);
    assert(cross_session.counters() == (std::pair<std::size_t, std::size_t>{0,0}));
    assert(cross_session.invariant());
    std::cout << "native-ipc-two-process-EOF PASS (NOT authenticated transport)\n";
#else
    std::cout << "native-ipc-two-process: unsupported target (Linux required)\n";
#endif
}
