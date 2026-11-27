// Host-only differential counterpart to Wasm's loopback continuation test.
// No compiled Atto guest, actor scheduler, or production reactor is exercised.
#include "oreslang/runtime/PinnedContinuationGate.hpp"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <cerrno>
#include <stdexcept>

using namespace oreslang::runtime;
namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) { check(fd >= 0, "socket creation failed"); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd() { close(); }
    void close() { if (value >= 0) { ::close(value); value = -1; } }
};
void ready(int fd, short events) {
    pollfd p{fd, events, 0};
    check(::poll(&p, 1, 2000) == 1 && (p.revents & events), "socket readiness timeout");
}
void send_byte(int fd, unsigned char byte) {
    ready(fd, POLLOUT);
    check(::send(fd, &byte, 1, 0) == 1, "send failed");
}
unsigned char receive_byte(int fd) {
    ready(fd, POLLIN);
    unsigned char byte = 0;
    check(::recv(fd, &byte, 1, 0) == 1, "receive failed");
    return byte;
}
void connect_local(int fd, const sockaddr_in& address) {
    check(::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
          "loopback connect failed");
}
void scenario(AsyncIoTerminal outcome) {
    Fd listener(::socket(AF_INET, SOCK_STREAM, 0));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(::bind(listener.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
          "loopback bind failed");
    check(::listen(listener.value, 2) == 0, "listen failed");
    socklen_t size = sizeof(address);
    check(::getsockname(listener.value, reinterpret_cast<sockaddr*>(&address), &size) == 0,
          "getsockname failed");
    Fd a(::socket(AF_INET, SOCK_STREAM, 0));
    connect_local(a.value, address);
    ready(listener.value, POLLIN);
    Fd peer_a(::accept(listener.value, nullptr, nullptr));
    Fd b(::socket(AF_INET, SOCK_STREAM, 0));
    connect_local(b.value, address);
    ready(listener.value, POLLIN);
    Fd peer_b(::accept(listener.value, nullptr, nullptr));

    const AsyncIoTicket ta{1, 1, 1}, tb{2, 1, 1};
    using Gate = BorrowerPinnedContinuationGate;
    Gate ga(ta), gb(tb);
    std::array<unsigned char, 1> ma{11}, mb{41};
    const auto loan = ga.acquire(ta, 1, Gate::Loan::exclusive);
    check(loan.has_value(), "A loan missing");
    send_byte(a.value, 1);
    check(receive_byte(peer_a.value) == 1, "A request wrong");
    check(ga.terminal() == AsyncIoTerminal::pending && !ga.try_claim_resume(ta), "A not pending");
    check(!gb.try_claim_resume(ta), "foreign resume admitted");

    send_byte(b.value, 2);
    check(receive_byte(peer_b.value) == 2, "B request wrong");
    send_byte(peer_b.value, 42);
    mb[0] = receive_byte(b.value);
    check(gb.settle(tb, AsyncIoTerminal::success), "B completion lost");
    pollfd pending{a.value, POLLIN, 0};
    check(::poll(&pending, 1, 0) == 0 && ga.terminal() == AsyncIoTerminal::pending,
          "A must remain pending while B progresses");
    b.close(); peer_b.close();
    check(gb.broker_acknowledge(tb) && gb.try_claim_resume(tb) && gb.finish_resume(tb)
          && gb.try_retire(tb), "B cleanup failed");
    check(ma[0] == 11, "A memory changed");

    check(ga.settle(ta, outcome), "terminal arbitration failed");
    check(!ga.acquire(ta, 2, Gate::Loan::shared) && !ga.try_claim_resume(ta),
          "terminal operation admitted loan or premature cleanup");
    send_byte(peer_a.value, 99);
    check(receive_byte(a.value) == 99, "late reply missing");
    check(!ga.settle(ta, AsyncIoTerminal::success) && ga.terminal() == outcome,
          "late reply overwrote terminal");
    check(ma[0] == 11, "late reply changed A memory");
    a.close(); peer_a.close();
    check(ga.broker_acknowledge(ta) && ga.try_claim_resume(ta) && !ga.try_claim_resume(ta),
          "cleanup claim not exactly once");
    check(ga.finish_resume(ta) && !ga.try_retire(ta), "live loan failed to pin retirement");
    check(!ga.release(ta, 2, *loan) && ga.release(ta, 1, *loan)
          && !ga.release(ta, 1, *loan) && ga.try_retire(ta) && !ga.try_retire(ta),
          "receipt retirement not exactly once");
    ma.fill(0);
    check(mb[0] == 42, "B memory not preserved");
    const AsyncIoTicket fresh{1, 2, 1};
    Gate replacement(fresh);
    check(!replacement.settle(ta, AsyncIoTerminal::success) && !replacement.try_claim_resume(ta)
          && replacement.terminal() == AsyncIoTerminal::pending, "old generation revived");
}
} // namespace

int main() {
    scenario(AsyncIoTerminal::cancelled);
    scenario(AsyncIoTerminal::timed_out);
}
