// Native integration slice, NOT a compiler for Spin's module graph.
// All actor calls go through NativeActorScheduler's bounded in-memory mailbox.
#include "oreslang/runtime/ActorOwnedI64Heap.h"
#include "oreslang/runtime/NativeActorScheduler.h"
#include "oreslang/runtime/NativeHttpExchange.hpp"
#include "oreslang/runtime/NativeSocketReadiness.hpp"
#include "oreslang/runtime/BoundedMpscQueue.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

extern "C" void oreslang_actor_HttpWorker_init(OresActorI64Heap*, uint64_t, uint64_t);
extern "C" int64_t oreslang_actor_HttpWorker_on_message(OresActorI64Heap*, uint64_t, uint64_t, int64_t);
using namespace oreslang::runtime;
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
using Clock = std::chrono::steady_clock;
std::uint64_t now() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count()); }
void require(bool ok) { if (!ok) std::abort(); }
void system_check(bool ok, const char* operation) { if (!ok) throw std::system_error(errno, std::generic_category(), operation); }
class Fd {
public:
    Fd() : fd_(-1) {}
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { close(); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return fd_; }
    int release() { int result = fd_; fd_ = -1; return result; }
    void set(int fd) { close(); fd_ = fd; }
    void close() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
private: int fd_;
};
void nonblocking(int fd) {
    system_check(::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK) >= 0, "O_NONBLOCK");
    system_check(::fcntl(fd, F_SETFD, FD_CLOEXEC) >= 0, "FD_CLOEXEC");
#if defined(__APPLE__)
    int one = 1;
    system_check(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one)) == 0, "SO_NOSIGPIPE");
#endif
}
std::string lower(std::string text) {
    for (char& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 'a' - 'A');
    return text;
}
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
// Deliberately narrow HTTP/1.1: no body, chunking, upgrades or pipelining.
// Invalid framing is rejected/closed by the transport, never interpreted as a
// second request. Valid 404/405 requests STILL pass to a guest actor.
bool parse(const std::string& bytes, int& status, bool& close) {
    auto end = bytes.find("\r\n\r\n");
    if (end == std::string::npos || end + 4 != bytes.size()) return false;
    auto line_end = bytes.find("\r\n");
    std::string_view line(bytes.data(), line_end);
    auto space = line.find(' '), last = line.rfind(' ');
    if (space == std::string_view::npos || last == space || line.substr(last + 1) != "HTTP/1.1") return false;
    auto method = line.substr(0, space), path = line.substr(space + 1, last - space - 1);
    if (method.empty() || path.empty() || path.front() != '/') return false;
    for (unsigned char c : line) if (c < 32 || c > 126) return false;
    for (unsigned char c : method) if (c < 'A' || c > 'Z') return false;
    status = method != "GET" ? 405 : path == "/health" ? 200 : 404;
    bool host = false, content_length = false, connection = false;
    std::size_t at = line_end + 2, headers = 0;
    while (at < end) {
        auto next = bytes.find("\r\n", at);
        std::string_view h(bytes.data() + at, next - at);
        auto colon = h.find(':');
        if (colon == std::string_view::npos || !colon || ++headers > 64) return false;
        auto key = lower(std::string(h.substr(0, colon)));
        for (unsigned char c : key) if (std::string_view("!#$%&'*+-.^_`|~0123456789abcdefghijklmnopqrstuvwxyz").find(static_cast<char>(c)) == std::string_view::npos) return false;
        auto value = trim(h.substr(colon + 1));
        for (unsigned char c : value) if ((c < 32 && c != '\t') || c == 127) return false;
        if (key == "host") { if (host || value.empty()) return false; host = true; }
        if (key == "content-length") { if (content_length || value != "0") return false; content_length = true; }
        if (key == "transfer-encoding" || key == "expect" || key == "upgrade") return false;
        if (key == "connection") {
            if (connection) return false;
            connection = true;
            auto v = lower(std::string(value));
            if (v != "close" && v != "keep-alive") return false;
            close = v == "close";
        }
        at = next + 2;
    }
    return host;
}
struct Connection {
    Fd fd;
    std::uint32_t incarnation = 0;
    bool used = false, pending = false, busy = false, close = false;
    std::string input, output;
    std::size_t sent = 0, actor = 0;
    int status = 200;
    std::uint64_t first_byte = 0, parsed = 0, started = 0, finished = 0, progress = 0;
    NativeHttpExchange exchange;
};
struct Actor {
    OresNativeActorRef ref{};
    OresActorI64Heap* heap = nullptr;
    std::uint64_t requests = 0;
};
class Server {
    static constexpr std::size_t max_connections = 1024;
    static constexpr std::uint64_t listener_token = UINT64_MAX;
    NativeSocketReadiness readiness;
    Fd listener;
    std::array<Connection, max_connections> connections{};
    std::vector<Actor> actors;
    BoundedMpscQueue<std::uint64_t, 2048> completions;
    OresNativeActorScheduler* scheduler = nullptr;
    std::size_t next_actor = 0, live = 0, peak = 0;
    std::uint64_t accepted = 0, completed = 0, bad_frames = 0, mailbox_retries = 0;
    std::uint64_t queue_ns = 0, actor_ns = 0, completion_ns = 0, exchange_ns = 0, errors = 0;
public:
    Server(unsigned port, std::size_t count, std::size_t carriers) : actors(count) {
        listener.set(::socket(AF_INET, SOCK_STREAM, 0));
        system_check(listener.get() >= 0, "socket"); nonblocking(listener.get());
        int one = 1;
        system_check(::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0, "SO_REUSEADDR");
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(static_cast<std::uint16_t>(port));
        system_check(::bind(listener.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        system_check(::listen(listener.get(), 1024) == 0, "listen");
        readiness.set(listener.get(), listener_token, true, false, false);
        scheduler = ores_native_actor_scheduler_open(count, 128, carriers, deliver, this);
        if (!scheduler) throw std::runtime_error("native scheduler allocation failed");
        for (std::size_t i = 0; i < count; ++i) {
            actors[i].ref = ores_native_actor_scheduler_spawn(scheduler, i);
            require(actors[i].ref.scheduler_domain != 0);
        }
        socklen_t length = sizeof(address);
        system_check(::getsockname(listener.get(), reinterpret_cast<sockaddr*>(&address), &length) == 0, "getsockname");
        // Bootstrap-only output; no stdout/stderr I/O during requests.
        std::cout << "LISTENING port=" << ntohs(address.sin_port) << " pid=" << getpid()
                  << " actors=" << count << " carriers=" << carriers << std::endl;
    }
    ~Server() {
        if (scheduler) {
            ores_native_actor_scheduler_retire(scheduler);
            require(ores_native_actor_scheduler_drain(scheduler));
            require(ores_native_actor_scheduler_try_destroy(scheduler));
        }
        for (auto& a : actors) if (a.heap) {
            ores_actor_i64_heap_retire(a.heap);
            require(ores_actor_i64_heap_try_destroy(a.heap));
        }
    }
    static void deliver(void* raw, std::size_t index, std::uint64_t payload) {
        auto& s = *static_cast<Server*>(raw);
        require(index < s.actors.size());
        auto& a = s.actors[index];
        require(ores_native_actor_scheduler_is_current_actor(s.scheduler, a.ref));
        auto slot = payload & 1023u;
        auto& c = s.connections[slot];
        NativeHttpExchange::Token token{payload >> 10, a.ref};
        require(c.exchange.claim(token));
        c.started = now();
        if (!a.heap) {
            a.heap = ores_actor_i64_heap_open(index + 1, 1, 1);
            require(a.heap != nullptr);
            auto turn = ores_actor_i64_heap_enter(a.heap, index + 1, 1);
            oreslang_actor_HttpWorker_init(a.heap, turn.domain, turn.epoch);
            ores_actor_i64_heap_leave(a.heap, turn);
        }
        auto turn = ores_actor_i64_heap_enter(a.heap, index + 1, 1);
        auto status = oreslang_actor_HttpWorker_on_message(a.heap, turn.domain, turn.epoch, c.status);
        require(status == c.status);
        require(ores_actor_i64_heap_field_read(a.heap, turn.domain, turn.epoch, 0)
                == static_cast<std::int64_t>(a.requests + 1));
        ores_actor_i64_heap_leave(a.heap, turn);
        ++a.requests;
        c.output = response(c.status, c.close, true, index);
        c.finished = now();
        require(c.exchange.complete(token));
        require(s.completions.try_push(payload)); // at most 1024 outstanding slots
        s.readiness.wake();
    }
    static std::string response(int status, bool close, bool actor, std::size_t index = 0) {
        const std::string body = status == 200 ? "ok\n" : std::to_string(status) + "\n";
        return "HTTP/1.1 " + std::to_string(status) + " Response\r\nContent-Length: " + std::to_string(body.size())
            + "\r\nConnection: " + (close ? "close" : "keep-alive")
            + (actor ? "\r\nX-Actor-Id: " + std::to_string(index + 1) + "\r\nX-Guest-AOT: 1" : "")
            + "\r\n\r\n" + body;
    }
    std::uint64_t event_token(std::size_t i) const { return (std::uint64_t(connections[i].incarnation) << 32) | (i + 1); }
    void interest(std::size_t i, bool read, bool write) {
        readiness.set(connections[i].fd.get(), event_token(i), read, write, true);
    }
    void release(std::size_t i) {
        auto& c = connections[i]; require(!c.busy);
        c.fd.close(); c.input.clear(); c.output.clear(); c.used = c.pending = false;
        --live;
    }
    void accept_ready() {
        for (unsigned n = 0; n < 256; ++n) {
            Fd client(::accept(listener.get(), nullptr, nullptr));
            if (client.get() < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                throw std::system_error(errno, std::generic_category(), "accept");
            }
            nonblocking(client.get());
            int one = 1;
            system_check(::setsockopt(client.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0, "TCP_NODELAY");
            auto it = std::find_if(connections.begin(), connections.end(), [](const auto& c) { return !c.used && c.incarnation != UINT32_MAX; });
            if (it == connections.end()) { ++errors; continue; }
            auto i = static_cast<std::size_t>(it - connections.begin());
            // dup is unnecessary: transfer ownership explicitly, not via a syscall.
            // Fd has no copy; release only for this accept->reactor ownership move.
            it->fd.set(client.release());
            ++it->incarnation; it->used = true; it->pending = it->busy = it->close = false;
            it->sent = 0; it->first_byte = 0; it->progress = now();
            ++live; peak = std::max(peak, live);
            readiness.set(it->fd.get(), event_token(i), true, false, false);
        }
    }
    void dispatch(std::size_t i) {
        auto& c = connections[i];
        if (!c.exchange.enqueue(actors[c.actor].ref)) throw std::runtime_error("exchange generation exhausted");
        auto token = c.exchange.token();
        if (token.generation > (UINT64_MAX >> 10)) throw std::runtime_error("mailbox token generation exhausted");
        auto payload = (token.generation << 10) | i;
        c.busy = true;
        auto result = ores_native_actor_scheduler_try_send(scheduler, actors[c.actor].ref, payload);
        if (result != ORES_NATIVE_SEND_ACCEPTED) {
            require(c.exchange.rollback(token)); c.busy = false;
            if (result != ORES_NATIVE_SEND_MAILBOX_FULL) throw std::runtime_error("actor mailbox unavailable");
            c.pending = true; ++mailbox_retries; return;
        }
        c.pending = false; ++accepted;
    }
    void read_ready(std::size_t i) {
        auto& c = connections[i];
        if (c.busy || c.pending || !c.output.empty()) return;
        std::array<char, 8192> buffer{};
        const auto n = ::recv(c.fd.get(), buffer.data(), buffer.size(), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
            release(i); return;
        }
        c.progress = now(); if (!c.first_byte) c.first_byte = c.progress;
        c.input.append(buffer.data(), static_cast<std::size_t>(n));
        if (c.input.size() > 8192 || c.input.find("\r\n\r\n") != std::string::npos) {
            c.parsed = now();
            if (c.input.size() > 8192 || !parse(c.input, c.status, c.close)) {
                ++bad_frames; c.status = 400; c.close = true; c.output = response(400, true, false); interest(i, false, true); return;
            }
            c.actor = next_actor++ % actors.size();
            interest(i, false, false); dispatch(i);
        }
    }
    void complete_ready() {
        std::uint64_t payload;
        while (completions.try_pop(payload)) {
            auto i = payload & 1023u; auto& c = connections[i];
            require(c.busy && c.exchange.reclaim({payload >> 10, actors[c.actor].ref}));
            queue_ns += c.started - c.parsed; actor_ns += c.finished - c.started;
            completion_ns += now() - c.finished;
            c.busy = false; c.progress = now();
            write_ready(i);
        }
    }
    void write_ready(std::size_t i) {
        auto& c = connections[i];
        if (c.busy || c.output.empty()) return;
#if defined(__linux__)
        constexpr int flags = MSG_NOSIGNAL;
#else
        constexpr int flags = 0;
#endif
        auto n = ::send(c.fd.get(), c.output.data() + c.sent, c.output.size() - c.sent, flags);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) { interest(i, false, true); return; }
            ++errors; release(i); return;
        }
        c.sent += static_cast<std::size_t>(n); c.progress = now();
        if (c.sent != c.output.size()) { interest(i, false, true); return; }
        if (!c.close || c.status != 400) { ++completed; exchange_ns += now() - c.first_byte; }
        if (c.close) { release(i); return; }
        c.input.clear(); c.output.clear(); c.sent = 0; c.first_byte = 0;
        interest(i, true, false);
    }
    void run() {
        std::array<NativeSocketReadiness::Event, NativeSocketReadiness::batch> events{};
        auto sweep = now();
        while (!stopping) {
            complete_ready();
            bool pending = false;
            for (std::size_t i = 0; i < connections.size(); ++i) if (connections[i].pending) { dispatch(i); pending = pending || connections[i].pending; }
            auto count = readiness.wait(events, pending ? 1 : 50);
            for (std::size_t e = 0; e < count; ++e) {
                const auto& event = events[e];
                if (event.token == listener_token) { accept_ready(); continue; }
                auto slot = (event.token & UINT32_MAX);
                if (!slot || slot > connections.size()) continue;
                auto i = static_cast<std::size_t>(slot - 1); auto& c = connections[i];
                if (!c.used || event.token != event_token(i) || c.busy) continue;
                if (event.error) { ++errors; release(i); continue; }
                if (event.readable) read_ready(i);
                if (c.used && event.writable) write_ready(i);
            }
            if (now() - sweep > 1000000000ull) {
                sweep = now();
                for (std::size_t i = 0; i < connections.size(); ++i) {
                    auto& c = connections[i];
                    if (c.used && !c.busy && sweep - c.progress > 15000000000ull) { ++errors; release(i); }
                }
            }
        }
        listener.close();
        // External lifecycle owner, never an actor carrier: fence callbacks,
        // reclaim every published response, then retire heaps in the destructor.
        ores_native_actor_scheduler_retire(scheduler);
        require(ores_native_actor_scheduler_drain(scheduler)); complete_ready();
        std::size_t used = 0; for (const auto& a : actors) if (a.requests) ++used;
        const double denominator = static_cast<double>(accepted ? accepted : 1) * 1000000.0;
        std::cout << "{\"accepted\":" << accepted << ",\"completed\":" << completed
            << ",\"scheduler_completed\":" << ores_native_actor_scheduler_completed(scheduler)
            << ",\"actors_used\":" << used << ",\"peak_connections\":" << peak
            << ",\"mailbox_retries\":" << mailbox_retries << ",\"bad_frames\":" << bad_frames
            << ",\"transport_errors\":" << errors << ",\"queue_mean_ms\":" << queue_ns / denominator
            << ",\"actor_mean_ms\":" << actor_ns / denominator << ",\"completion_mean_ms\":" << completion_ns / denominator
            << ",\"server_mean_ms\":" << exchange_ns / denominator << "}" << std::endl;
    }
};
unsigned number(const char* text, unsigned maximum) {
    std::string s(text); std::size_t consumed = 0; auto n = std::stoul(s, &consumed);
    if (consumed != s.size() || n > maximum) throw std::invalid_argument("invalid numeric argument");
    return static_cast<unsigned>(n);
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("usage: native-http-server PORT ACTORS CARRIERS");
        auto port = number(argv[1], 65535), actors = number(argv[2], 1024), carriers = number(argv[3], 64);
        if (!actors || !carriers) throw std::invalid_argument("actors/carriers must be positive");
        std::signal(SIGTERM, stop); std::signal(SIGINT, stop);
        auto server = std::make_unique<Server>(port, actors, carriers); server->run();
    } catch (const std::exception& e) { std::cerr << e.what() << std::endl; return 1; }
}
