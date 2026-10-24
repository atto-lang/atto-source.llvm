#include "oreslang/runtime/NativeEpollTransportBridge.hpp"
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <dirent.h>
#include <optional>
#include <thread>
#include <utility>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <unistd.h>

using namespace oreslang::runtime;
using Bridge = NativeEpollTransportBridge;
static std::optional<Bridge::Ticket> watch(Bridge& b, int fd) {
    for(int tries = 0; tries < 3000; ++tries) {
        auto r=b.try_watch(fd,EPOLLIN);
        if(r) return r;
        std::this_thread::yield();
    }
    return {};
}
static std::optional<Bridge::CompletionLease> pop(Bridge& b) {
    for(int tries = 0; tries < 3000; ++tries) {
        auto r=b.try_pop();
        if(r) return r;
        std::this_thread::yield();
    }
    return {};
}
static std::size_t open_fd_count() {
    DIR* directory = ::opendir("/proc/self/fd");
    assert(directory);
    std::size_t count = 0;
    while (const auto* entry = ::readdir(directory)) {
        if (entry->d_name[0] == '.' &&
            (entry->d_name[1] == '\0' ||
             (entry->d_name[1] == '.' && entry->d_name[2] == '\0'))) continue;
        ++count;
    }
    assert(::closedir(directory) == 0);
    return count;
}
int main() {
    const auto initial_fds = open_fd_count();
    for (int iteration = 0; iteration < 12; ++iteration) {
        NativeTransportCallbackGate gate(1,1,1,static_cast<uint64_t>(iteration+1));
        std::array<std::array<int,2>,Bridge::Reactor::capacity+1> fds{};
        for(auto& pair:fds) {
            pair={-1,-1};
            assert(::socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair.data())==0);
        }
        std::array<Bridge::Ticket, Bridge::Reactor::capacity> tickets{};
        {
            Bridge b(gate);
            assert(b.start());
            for(size_t i=0;i<tickets.size();++i) {
                auto x=watch(b,fds[i][0]);
                assert(x);
                tickets[i]=*x;
            }
            assert(!b.try_watch(fds.back()[0], EPOLLIN));
            b.stop();
            assert(!gate.try_drain());
            std::array<std::optional<Bridge::CompletionLease>, Bridge::Reactor::capacity> held{};
            std::array<bool, Bridge::Reactor::capacity> seen{};
            for(size_t i=0;i<tickets.size();++i) {
                auto event=pop(b); assert(event);
                assert(event->event.outcome==Bridge::Reactor::Outcome::stopped);
                auto found=false;
                for(size_t k=0;k<tickets.size();++k) {
                    if (tickets[k].value==event->event.ticket.value) {
                        assert(!seen[k]); seen[k]=true; found=true; break;
                    }
                }
                assert(found);
                held[i].emplace(std::move(*event));
                assert(!gate.try_drain());
            }
            assert(!pop(b));
            for (auto& h:held) h.reset();
            auto proof=gate.try_drain();assert(proof && gate.verifies(*proof));
        }
        for(auto& pair:fds) {::close(pair[0]);::close(pair[1]);}
    }
    // Old ticket is invalid after the completed watch slot is reused.
    int fds[2];
    assert(::socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,fds)==0);
    NativeTransportCallbackGate gate(2,3,4,5);
    {
        Bridge b(gate);
        assert(b.start());
        auto first=watch(b,fds[0]);assert(first);
        bool cancelled=false;
        for(int i=0;i<3000&&!cancelled;++i){cancelled=b.try_cancel(*first);std::this_thread::yield();}
        assert(cancelled);
        auto lease=pop(b);assert(lease);
        assert(lease->event.ticket.value==first->value);
        auto second=watch(b,fds[0]);assert(second && second->value!=first->value);
        assert(!b.try_cancel(*first));
        b.stop();
        auto stopped=pop(b);assert(stopped && stopped->event.ticket.value==second->value);
        assert(stopped->event.outcome==Bridge::Reactor::Outcome::stopped);
        assert(!gate.try_drain());
        lease.reset();
        assert(!gate.try_drain());
        stopped.reset();
        assert(gate.try_drain());
    }
    ::close(fds[0]);::close(fds[1]);
    // Concurrent stop versus admission: an accepted watch is always
    // accompanied by its permit, and any rejected attempt leaks none.
    for (int attempt = 0; attempt < 96; ++attempt) {
        int pair[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair)==0);
        NativeTransportCallbackGate race_gate(12, 10, 8, static_cast<uint64_t>(attempt+100));
        {
            Bridge b(race_gate);
            assert(b.start());
            std::atomic<bool> go{false};
            std::optional<Bridge::Ticket> accepted;
            std::thread producer([&] {
                while(!go.load(std::memory_order_acquire))std::this_thread::yield();
                for(int n=0;n<1000;++n) {
                    accepted=b.try_watch(pair[0],EPOLLIN);
                    if(accepted || !race_gate.accepting())break;
                    std::this_thread::yield();
                }
            });
            go.store(true,std::memory_order_release);
            std::this_thread::yield();
            b.stop();
            producer.join();
            assert(!b.try_watch(pair[0],EPOLLIN));
            if(accepted) {
                assert(!race_gate.try_drain());
                auto terminal=pop(b); assert(terminal);
                assert(terminal->event.ticket.value==accepted->value);
                assert(terminal->event.outcome==Bridge::Reactor::Outcome::stopped);
                assert(!race_gate.try_drain());
                terminal.reset();
            }
            assert(race_gate.try_drain());
        }
        ::close(pair[0]);::close(pair[1]);
    }
    // Concurrent cancel versus reactor stop: exactly one terminal result.
    for (int attempt = 0; attempt < 96; ++attempt) {
        int pair[2];
        assert(::socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0,pair)==0);
        NativeTransportCallbackGate race_gate(13, 11, 9, static_cast<uint64_t>(attempt+300));
        {
            Bridge b(race_gate);
            assert(b.start());
            auto ticket=watch(b,pair[0]); assert(ticket);
            std::atomic<bool> go{false};
            std::thread canceller([&] {
                while(!go.load(std::memory_order_acquire))std::this_thread::yield();
                (void)b.try_cancel(*ticket);
            });
            go.store(true,std::memory_order_release);
            b.stop();
            canceller.join();
            auto terminal=pop(b);assert(terminal);
            assert(terminal->event.ticket.value==ticket->value);
            assert(terminal->event.outcome==Bridge::Reactor::Outcome::cancelled ||
                   terminal->event.outcome==Bridge::Reactor::Outcome::stopped);
            assert(!pop(b));
            assert(!race_gate.try_drain());
            terminal.reset();
            assert(race_gate.try_drain());
        }
        ::close(pair[0]);::close(pair[1]);
    }
    assert(open_fd_count() == initial_fds); // no epoll/eventfd/socket leaks
    std::puts("NATIVE_EPOLL_CAPACITY_AND_REPLAY_OK");
}
