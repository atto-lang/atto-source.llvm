#include <atomic>
#include <cassert>
#include <cstdlib>
#include <new>
#include <string>

static std::atomic<int> fails_after{-1};

void* operator new(std::size_t size) {
    const int previous = fails_after.load();
    if (previous >= 0 && fails_after.fetch_sub(1) == 0)
        throw std::bad_alloc();
    if (void* memory = std::malloc(size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

#include "oreslang/runtime/NativeIpcAuthenticatedStreamSession.hpp"

#include <sys/socket.h>
#include <unistd.h>

using namespace oreslang::runtime;

static NativeIpcWireV3::Bytes key() {
    NativeIpcWireV3::Bytes result(32u);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>(0x51u + i);
    return result;
}

static NativeIpcWireV3::ConnectionBinding binding() {
    NativeIpcWireV3::ConnectionBinding result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<unsigned char>(0x61u + i);
    return result;
}

int main() {
    int fds[2]{-1, -1};
    assert(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);

    NativeIpcWireV3::Bytes payload{'f','a','i','l'};
    IpcReceiptEnvelope envelope;
    envelope.domain = "tenant";
    envelope.sender = "sender";
    envelope.destination = "worker";
    envelope.session = "retireFail";
    envelope.payload_sha256 = NativeIpcWireV3::sha256_hex(payload);
    envelope.sender_incarnation = 1;
    envelope.destination_incarnation = 2;
    envelope.route_epoch = 3;
    envelope.capability_generation = 4;
    envelope.message_sequence = 1;
    envelope.frame_sequence = 1;

    const auto mac_key = key();
    const auto connection_binding = binding();
    NativeIpcAuthenticatedStreamSession session(
        OwnedSocketFd{fds[0]}, envelope, 101, 1,
        mac_key, connection_binding);

    const auto sent = session.send_nonblocking(envelope, payload);
    assert(sent == NativeIpcAuthenticatedStreamSession::SendStatus::complete ||
           sent == NativeIpcAuthenticatedStreamSession::SendStatus::would_block);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{1, 1}));

    // The retirement snapshot reserves one LostCorrelation. Force that first
    // post-close allocation to fail: physical authority must remain fenced,
    // but the correlation table must stay intact for an explicit retry.
    fails_after.store(0);
    bool allocation_failed = false;
    try {
        session.close_and_retire();
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    fails_after.store(-1);

    assert(allocation_failed);
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::quarantined);
    assert(session.key_zeroized());
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{1, 1}));
    assert(session.invariant());
    assert(session.send_nonblocking(envelope, payload) ==
        NativeIpcAuthenticatedStreamSession::SendStatus::retired);

    session.retry_quarantined_retire();
    assert(session.phase() ==
        NativeIpcAuthenticatedStreamSession::Phase::retired);
    auto losses = session.take_losses();
    assert(losses.size() == 1);
    assert(losses[0].envelope == envelope);
    assert(losses[0].last_observed ==
        IpcReceiptStage::dispatch_committed);
    assert(session.counters() ==
        (std::pair<std::size_t, std::size_t>{0, 0}));
    assert(session.invariant());

    ::close(fds[1]);
}
