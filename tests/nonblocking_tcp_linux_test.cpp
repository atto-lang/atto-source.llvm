#include "oreslang/runtime/NonblockingTcpLinux.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <limits>
using namespace oreslang::runtime;
static void check(bool yes) { if (!yes) throw std::runtime_error("nonblocking TCP proof failed"); }
int main() {
    auto listener = NonblockingTcpListener::loopback(0, 32);
    check(listener.local_port() > 0);
    auto none = listener.try_accept();
    check(none.would_block && !none.client.valid());
    auto client = OwnedSocketFd::create_tcp();
    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = ::htons(listener.local_port());
    peer.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    const int c = ::connect(client.get(), reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    check(c == 0 || (c == -1 && errno == EINPROGRESS));
    EpollReadiness reactor;
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    reactor.add(listener.fd(), EPOLLIN, 12345);
    reactor.wakeup();
    // No phantom user event on a cross-thread command notification.
    check(reactor.wait(ready, 500) == 0);
    std::thread notifier([&reactor] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        reactor.wakeup();
    });
    const auto started = std::chrono::steady_clock::now();
    check(reactor.wait(ready, 3000) == 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    notifier.join();
    check(elapsed < std::chrono::seconds(2));
    auto n = reactor.wait(ready, 1500);
    check(n > 0 && ready[0].token == 12345 && (ready[0].events & EPOLLIN));
    check(reactor.wait(ready, 0) == 0); // EPOLLONESHOT: no repeated accept notification
    auto accepted = listener.try_accept();
    check(!accepted.would_block && accepted.error_number == 0 && accepted.client.valid());
    check((::fcntl(accepted.client.get(), F_GETFL) & O_NONBLOCK) != 0);
    check((::fcntl(accepted.client.get(), F_GETFD) & FD_CLOEXEC) != 0);
    std::array<char, 64> bytes{};
    auto read = try_read(accepted.client.get(), bytes.data(), bytes.size());
    check(read.disposition == IoDisposition::would_block);
    reactor.add(accepted.client.get(), EPOLLIN, 98765);
    auto sent = try_write(client.get(), "hello", 5);
    check(sent.disposition == IoDisposition::transferred && sent.size == 5);
    n = reactor.wait(ready, 1500);
    bool found = false;
    for (std::size_t i=0; i<n; ++i) if (ready[i].token == 98765 && (ready[i].events & EPOLLIN)) found = true;
    check(found);
    check(reactor.wait(ready, 0) == 0); // unread bytes cannot spin an unarmed reactor
    reactor.rearm(accepted.client.get(), EPOLLIN, 98766);
    n = reactor.wait(ready, 1500);
    found = false;
    for (std::size_t i=0; i<n; ++i) if (ready[i].token == 98766 && (ready[i].events & EPOLLIN)) found = true;
    check(found); // rearm may change identity after generation validation
    read = try_read(accepted.client.get(), bytes.data(), bytes.size());
    check(read.disposition == IoDisposition::transferred && read.size == 5);
    check(std::memcmp(bytes.data(), "hello", 5) == 0);
    auto answer = try_write(accepted.client.get(), "ok", 2);
    check(answer.disposition == IoDisposition::transferred && answer.size == 2);
    std::array<char, 2> reply{};
    read = try_read(client.get(), reply.data(), reply.size());
    check(read.disposition == IoDisposition::transferred && read.size == 2);
    check(std::memcmp(reply.data(), "ok", 2) == 0);
    bool rejected = false;
    try { reactor.rearm(accepted.client.get(), EPOLLIN, EpollReadiness::wake_token); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected);
    reactor.remove(accepted.client.get());
    reactor.remove(listener.fd());
    check(try_read(-1, bytes.data(), bytes.size()).disposition == IoDisposition::error);
    check(try_write(-1, "x", 1).disposition == IoDisposition::error);
    return 0;
}
