#include "oreslang/runtime/NonblockingTcpLinux.hpp"
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
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
    reactor.add(listener.fd(), EPOLLIN, 12345);
    std::array<EpollReadiness::Ready, EpollReadiness::max_batch> ready{};
    auto n = reactor.wait(ready, 1500);
    check(n > 0 && ready[0].token == 12345 && (ready[0].events & EPOLLIN));
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
    read = try_read(accepted.client.get(), bytes.data(), bytes.size());
    check(read.disposition == IoDisposition::transferred && read.size == 5);
    check(std::memcmp(bytes.data(), "hello", 5) == 0);
    auto answer = try_write(accepted.client.get(), "ok", 2);
    check(answer.disposition == IoDisposition::transferred && answer.size == 2);
    std::array<char, 2> reply{};
    read = try_read(client.get(), reply.data(), reply.size());
    check(read.disposition == IoDisposition::transferred && read.size == 2);
    check(std::memcmp(reply.data(), "ok", 2) == 0);
    reactor.remove(accepted.client.get());
    reactor.remove(listener.fd());
    check(try_read(-1, bytes.data(), bytes.size()).disposition == IoDisposition::error);
    check(try_write(-1, "x", 1).disposition == IoDisposition::error);
    return 0;
}
