#include "oreslang/runtime/NativeQuicLoopbackUdp.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <thread>
#include <chrono>

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
    for (int i=0; i<1000; ++i) {
        got=second.receive(input.data(), 2);
        if (got.outcome != Sock::Outcome::WouldBlock) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(got.outcome == Sock::Outcome::Invalid);
    first.close();
    assert(first.send_to_loopback(second.port(), ping, sizeof ping) == Sock::Outcome::Closed);
    assert(first.receive(input.data(), input.size()).outcome == Sock::Outcome::Closed);
    assert(second.receive(nullptr, input.size()).outcome == Sock::Outcome::Invalid);
    second.close();
    std::cout << "Native QUIC loopback UDP: bounded nonblocking send/recv passed (not QUIC)\n";
}
