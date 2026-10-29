#include "oreslang/runtime/NativeQuicLoopbackUdp.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace oreslang::native_quic;
int main() {
    using Sock = NativeQuicLoopbackUdp;
    Sock first, second;
    assert(first.open());
    assert(!first.open());
    assert(second.open());
    assert(first.port() != 0 && second.port() != 0 && first.port() != second.port());
    std::uint8_t ping[] = {0x70, 0x69, 0x6e, 0x67};
    assert(first.send_to_loopback(0, ping, sizeof ping) == Sock::Outcome::Invalid);
    assert(first.send_to_loopback(second.port(), nullptr, sizeof ping) == Sock::Outcome::Invalid);
    assert(first.send_to_loopback(second.port(), ping, Sock::MaxDatagram + 1) == Sock::Outcome::Invalid);
    assert(first.send_to_loopback(second.port(), ping, sizeof ping) == Sock::Outcome::Ok);
    std::array<std::uint8_t, Sock::MaxDatagram> input{};
    Sock::Received got{Sock::Outcome::WouldBlock,0,0};
    for (int i = 0; i < 1000 && got.outcome == Sock::Outcome::WouldBlock; ++i) {
        got = second.receive(input.data(), input.size());
        if (got.outcome == Sock::Outcome::WouldBlock)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Ok);
    assert(got.size == sizeof ping && got.source_port == first.port());
    for (std::size_t i=0; i<got.size; ++i) assert(input[i] == ping[i]);
    assert(first.send_to_loopback(second.port(), ping, sizeof ping) == Sock::Outcome::Ok);
    input.fill(0x6b);
    for (int i=0; i<1000; ++i) {
        got=second.receive(input.data(), 2);
        if (got.outcome != Sock::Outcome::WouldBlock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Invalid);
    for (auto value : input) assert(value == 0x6b);
    // The rejected datagram is consumed exactly once; no stale prefix or
    // rejected packet may remain queued ahead of subsequent traffic.
    assert(second.receive(input.data(), input.size()).outcome == Sock::Outcome::WouldBlock);
    // Boundary: the full 1350-byte datagram must survive untruncated.
    std::array<std::uint8_t, Sock::MaxDatagram> full{};
    full.fill(0xa5);
    assert(first.send_to_loopback(second.port(), full.data(), full.size()) == Sock::Outcome::Ok);
    for (int i=0; i<1000; ++i) {
        got=second.receive(input.data(), input.size());
        if (got.outcome != Sock::Outcome::WouldBlock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Ok && got.size == full.size());
    for (auto value : input) assert(value == 0xa5);

    // Independent raw socket sends a datagram above the cap. Linux MSG_TRUNC
    // must report invalid even when the supplied output buffer is smaller.
    const int raw = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    assert(raw >= 0);
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(second.port());
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    std::array<std::uint8_t, Sock::MaxDatagram + 1> oversized{};
    assert(::sendto(raw, oversized.data(), oversized.size(), 0,
                    reinterpret_cast<sockaddr*>(&dst), sizeof(dst)) ==
           static_cast<ssize_t>(oversized.size()));
    input.fill(0x3c);
    for (int i=0; i<1000; ++i) {
        got=second.receive(input.data(), input.size());
        if (got.outcome != Sock::Outcome::WouldBlock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Invalid);
    for (auto value : input) assert(value == 0x3c);
    assert(second.receive(input.data(), input.size()).outcome == Sock::Outcome::WouldBlock);
    // Recovery: an oversize rejection must not poison the socket or reorder
    // the next valid datagram.
    assert(first.send_to_loopback(second.port(), ping, sizeof ping) == Sock::Outcome::Ok);
    for (int i=0; i<1000; ++i) {
        got=second.receive(input.data(), input.size());
        if (got.outcome != Sock::Outcome::WouldBlock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Ok);
    assert(got.size == sizeof ping && got.source_port == first.port());
    for (std::size_t i=0; i<got.size; ++i) assert(input[i] == ping[i]);
    assert(::close(raw) == 0);
    first.close();
    assert(first.send_to_loopback(second.port(), ping, sizeof ping) == Sock::Outcome::Closed);
    assert(first.receive(input.data(), input.size()).outcome == Sock::Outcome::Closed);
    assert(second.receive(nullptr, input.size()).outcome == Sock::Outcome::Invalid);
    second.close();
    std::cout << "Native QUIC loopback UDP: bounded nonblocking send/recv passed (not QUIC)\n";
}
