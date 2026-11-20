// Real loopback-socket tests for ActorSharedTcpPool + ActorHttpAgent.
// Host-only: these are native runtime sockets, NOT compiled .atto guest code.
#include "oreslang/runtime/ActorHttpAgent.hpp"

#include <dirent.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace oreslang::runtime;
using namespace std::chrono_literals;

static int failures = 0;
#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)
#define CHECK_STATUS(got, want)                                                          \
    do {                                                                                 \
        const auto g_ = (got);                                                           \
        if (g_ != (want)) {                                                              \
            std::fprintf(stderr, "FAIL %s:%d: status %s, want %s\n", __FILE__, __LINE__, \
                         to_string(g_), to_string(want));                                \
            ++failures;                                                                  \
        }                                                                                \
    } while (0)

// ---------------------------------------------------------------------------
// Loopback HTTP/1.1 server. One thread per connection, keep-alive by default.
// The path selects the behaviour under test.
// ---------------------------------------------------------------------------
class LoopbackServer {
public:
    LoopbackServer() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || ::listen(listen_fd_, 128) != 0) {
            std::perror("bind/listen");
            std::abort();
        }
        socklen_t len = sizeof a;
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&a), &len);
        port_ = ntohs(a.sin_port);
        acceptor_ = std::thread([this] { accept_loop(); });
    }
    ~LoopbackServer() {
        stop_.store(true);
        acceptor_.join();
        std::lock_guard<std::mutex> g(mu_);
        for (auto& t : workers_) t.join();
        ::close(listen_fd_);
    }
    PoolOrigin origin(std::uint64_t tenant = 1) const { return {tenant, INADDR_LOOPBACK, port_}; }
    int accepted() const { return accepted_.load(); }
    // Server closes every open connection (simulates keep-alive timeout).
    void drop_all() {
        drop_epoch_.fetch_add(1);
        std::this_thread::sleep_for(60ms);
    }

private:
    void accept_loop() {
        while (!stop_.load()) {
            pollfd p{listen_fd_, POLLIN, 0};
            if (::poll(&p, 1, 20) <= 0) continue;
            int c = ::accept(listen_fd_, nullptr, nullptr);
            if (c < 0) continue;
#ifdef SO_NOSIGPIPE
            int one = 1;
            ::setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
            accepted_.fetch_add(1);
            std::lock_guard<std::mutex> g(mu_);
            workers_.emplace_back([this, c] { serve(c); });
        }
    }

    static void send_str(int fd, const std::string& s) {
        std::size_t off = 0;
        while (off < s.size()) {
#ifdef MSG_NOSIGNAL
            const ssize_t w = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
#else
            const ssize_t w = ::send(fd, s.data() + off, s.size() - off, 0);
#endif
            if (w <= 0) return;
            off += static_cast<std::size_t>(w);
        }
    }

    static std::string header_value(const std::string& head, const std::string& name) {
        std::string lower = head;
        for (auto& c : lower) c = static_cast<char>(std::tolower((unsigned char)c));
        std::string key = "\r\n" + name + ":";
        auto at = lower.find(key);
        if (at == std::string::npos) return {};
        auto start = at + key.size();
        auto end = head.find("\r\n", start);
        std::string v = head.substr(start, end - start);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        return v;
    }

    static std::string ok(const std::string& body, const std::string& extra = "") {
        return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\n" + extra + "\r\n" + body;
    }

    void serve(int fd) {
        const int my_epoch = drop_epoch_.load();
        std::string buf;
        char tmp[4096];
        bool open = true;
        while (open && !stop_.load()) {
            if (drop_epoch_.load() != my_epoch) break;
            pollfd p{fd, POLLIN, 0};
            if (::poll(&p, 1, 10) <= 0) continue;
            const ssize_t n = ::recv(fd, tmp, sizeof tmp, 0);
            if (n <= 0) break;
            buf.append(tmp, static_cast<std::size_t>(n));
            for (;;) {
                const auto end = buf.find("\r\n\r\n");
                if (end == std::string::npos) break;
                const std::string head = buf.substr(0, end + 2);
                const std::string cl = header_value(head, "content-length");
                const std::size_t body_len = cl.empty() ? 0 : std::stoul(cl);
                if (buf.size() < end + 4 + body_len) break;
                buf.erase(0, end + 4 + body_len);
                const auto sp1 = head.find(' ');
                const auto sp2 = head.find(' ', sp1 + 1);
                const std::string method = head.substr(0, sp1);
                const std::string path = head.substr(sp1 + 1, sp2 - sp1 - 1);
                const std::string id = header_value(head, "x-req");
                const std::string echo = "id=" + id + ";m=" + method + ";p=" + path + ";b=" + std::to_string(body_len);
                if (path == "/echo") {
                    send_str(fd, ok(echo));
                } else if (path == "/chunked") {
                    // Fragmented chunked body, split mid-size-line and mid-CRLF.
                    const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                        + [&] { char h[32]; std::snprintf(h, sizeof h, "%zx", echo.size()); return std::string(h); }()
                        + ";ext=1\r\n" + echo + "\r\n0\r\nX-Trailer: t\r\n\r\n";
                    for (std::size_t i = 0; i < wire.size(); i += 7) {
                        send_str(fd, wire.substr(i, 7));
                        std::this_thread::sleep_for(1ms);
                    }
                } else if (path == "/close") {
                    send_str(fd, ok(echo, "Connection: close\r\n"));
                    open = false;
                    break;
                } else if (path == "/double") {
                    send_str(fd, ok(echo) + ok("STRAY-FOR-NEXT-ACTOR"));
                } else if (path == "/late-extra") {
                    send_str(fd, ok(echo));
                    std::this_thread::sleep_for(30ms);
                    send_str(fd, ok("LATE-STRAY"));
                } else if (path == "/malformed") {
                    send_str(fd, "HTTP/1.1 2x0 OK\r\nContent-Length: 0\r\n\r\n");
                } else if (path == "/smuggle") {
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n");
                } else if (path == "/dup-cl") {
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nabc");
                } else if (path == "/bare-lf") {
                    send_str(fd, "HTTP/1.1 200 OK\nContent-Length: 0\n\n");
                } else if (path == "/big") {
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 2097152\r\n\r\n");
                } else if (path == "/slow") {
                    for (int i = 0; i < 40 && !stop_.load(); ++i) std::this_thread::sleep_for(10ms);
                    send_str(fd, ok(echo));
                } else if (path == "/hangup") {
                    open = false;
                    break;
                } else if (path == "/eof-body") {
                    send_str(fd, "HTTP/1.1 200 OK\r\n\r\n" + echo);
                    open = false;
                    break;
                } else if (path == "/interim") {
                    send_str(fd, "HTTP/1.1 100 Continue\r\n\r\n" + ok(echo));
                } else if (path == "/head") {
                    send_str(fd, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\n");
                } else if (path == "/http10") {
                    send_str(fd, "HTTP/1.0 200 OK\r\nContent-Length: " + std::to_string(echo.size()) + "\r\n\r\n" + echo);
                } else {
                    send_str(fd, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
                }
            }
        }
        ::close(fd);
    }

    int listen_fd_ = -1;
    std::uint16_t port_ = 0;
    std::atomic<bool> stop_{false};
    std::atomic<int> accepted_{0};
    std::atomic<int> drop_epoch_{0};
    std::thread acceptor_;
    std::mutex mu_;
    std::vector<std::thread> workers_;
};

static HttpRequestSpec get(const std::string& path, const std::string& id) {
    HttpRequestSpec r;
    r.target = path;
    r.host = "loopback.test";
    r.headers.push_back({"X-Req", id});
    return r;
}

static std::string body_of(const HttpAgentResult& r) { return std::string(r.response.body.begin(), r.response.body.end()); }

static int open_fd_count() {
    int n = 0;
#ifdef __APPLE__
    DIR* d = ::opendir("/dev/fd");
#else
    DIR* d = ::opendir("/proc/self/fd");
#endif
    if (!d) return -1;
    while (auto* e = ::readdir(d)) n += e->d_name[0] != '.';
    ::closedir(d);
    return n;
}

static const PoolActor A{101, 1}, B{202, 1}, A_restarted{101, 2};

static void test_sequential_reuse_across_actors() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    auto r1 = agent.request(A, srv.origin(), get("/echo", "a1"));
    CHECK_STATUS(r1.status, PoolStatus::ok);
    CHECK(body_of(r1) == "id=a1;m=GET;p=/echo;b=0");
    CHECK(!r1.reused_connection && r1.pooled_after);
    auto r2 = agent.request(B, srv.origin(), get("/echo", "b1"));
    CHECK_STATUS(r2.status, PoolStatus::ok);
    CHECK(body_of(r2) == "id=b1;m=GET;p=/echo;b=0");
    CHECK(r2.reused_connection);
    auto r3 = agent.request(A, srv.origin(), get("/chunked", "a2"));
    CHECK_STATUS(r3.status, PoolStatus::ok);
    CHECK(body_of(r3) == "id=a2;m=GET;p=/chunked;b=0");
    CHECK(r3.reused_connection && r3.pooled_after);
    auto r4 = agent.request(B, srv.origin(), get("/interim", "b2"));
    CHECK_STATUS(r4.status, PoolStatus::ok);
    CHECK(r4.response.status == 200 && body_of(r4) == "id=b2;m=GET;p=/interim;b=0");
    HttpRequestSpec head = get("/head", "h");
    head.method = "HEAD";
    auto r5 = agent.request(A, srv.origin(), head);
    CHECK_STATUS(r5.status, PoolStatus::ok);
    CHECK(r5.response.body.empty() && r5.pooled_after);
    HttpRequestSpec post = get("/echo", "p");
    post.method = "POST";
    post.body = {'x', 'y', 'z'};
    auto r6 = agent.request(B, srv.origin(), post);
    CHECK(body_of(r6) == "id=p;m=POST;p=/echo;b=3");
    CHECK(srv.accepted() == 1);
    CHECK(pool.stats().connects == 1 && pool.stats().reuses == 5);
}

static void test_lease_is_bound_to_actor_and_generation() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    auto a = pool.acquire(A, srv.origin());
    CHECK_STATUS(a.status, PoolStatus::ok);
    // Lease forwarded to B through a mailbox: B cannot use or release it.
    CHECK_STATUS(pool.release(B, a.lease, true), PoolStatus::wrong_actor);
    PoolLease forged = a.lease;
    forged.actor = B;
    CHECK_STATUS(pool.release(B, forged, true), PoolStatus::wrong_actor);
    // A restarted incarnation does not inherit the old incarnation's lease.
    CHECK_STATUS(pool.release(A_restarted, a.lease, true), PoolStatus::wrong_actor);
    PoolLease bad_slot = a.lease;
    bad_slot.slot = 9999;
    CHECK_STATUS(pool.release(A, bad_slot, true), PoolStatus::invalid_lease);
    CHECK_STATUS(pool.release(A, a.lease, true), PoolStatus::ok);
    // Stale: released lease cannot be released again ...
    CHECK_STATUS(pool.release(A, a.lease, true), PoolStatus::invalid_lease);
    // ... nor used after the same physical connection is re-leased to B.
    auto b = pool.acquire(B, srv.origin());
    CHECK(b.status == PoolStatus::ok && b.lease.slot == a.lease.slot && b.lease.reused);
    CHECK(b.lease.generation != a.lease.generation);
    CHECK_STATUS(pool.release(A, a.lease, false), PoolStatus::invalid_lease);
    CHECK(pool.stats().leased == 1);  // B's lease untouched by A's stale attempts
    CHECK_STATUS(pool.release(B, b.lease, true), PoolStatus::ok);
    CHECK_STATUS(pool.acquire(PoolActor{0, 0}, srv.origin()).status, PoolStatus::wrong_actor);
}

static void test_unsafe_connections_are_never_pooled() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    // Connection: close
    auto r = agent.request(A, srv.origin(), get("/close", "c"));
    CHECK(r.status == PoolStatus::ok && !r.pooled_after);
    // HTTP/1.0 and close-delimited bodies
    r = agent.request(A, srv.origin(), get("/http10", "z"));
    CHECK(r.status == PoolStatus::ok && !r.pooled_after);
    r = agent.request(A, srv.origin(), get("/eof-body", "e"));
    CHECK(r.status == PoolStatus::ok && !r.pooled_after && body_of(r) == "id=e;m=GET;p=/eof-body;b=0");
    // Extra response already in the buffer: A gets its own, the stray is dropped.
    r = agent.request(A, srv.origin(), get("/double", "d"));
    CHECK(r.status == PoolStatus::ok && !r.pooled_after && body_of(r) == "id=d;m=GET;p=/double;b=0");
    auto b = agent.request(B, srv.origin(), get("/echo", "b"));
    CHECK(b.status == PoolStatus::ok && body_of(b) == "id=b;m=GET;p=/echo;b=0");
    // Stray response arriving AFTER release: idle probe must discard it so B
    // never reads A's leftover.
    r = agent.request(A, srv.origin(), get("/late-extra", "l"));
    CHECK(r.status == PoolStatus::ok && r.pooled_after);
    std::this_thread::sleep_for(80ms);
    const auto discarded_before = pool.stats().discarded_on_probe;
    b = agent.request(B, srv.origin(), get("/echo", "b2"));
    CHECK(b.status == PoolStatus::ok && body_of(b) == "id=b2;m=GET;p=/echo;b=0");
    CHECK(pool.stats().discarded_on_probe > discarded_before);
    // Malformed / ambiguous framing => error, connection closed.
    for (const char* path : {"/malformed", "/smuggle", "/dup-cl", "/bare-lf"}) {
        r = agent.request(A, srv.origin(), get(path, "m"));
        CHECK_STATUS(r.status, PoolStatus::protocol_error);
        CHECK(!r.pooled_after);
    }
    r = agent.request(A, srv.origin(), get("/big", "big"));
    CHECK_STATUS(r.status, PoolStatus::too_large);
    r = agent.request(A, srv.origin(), get("/hangup", "h"));
    CHECK(r.status == PoolStatus::peer_closed && !r.pooled_after);
    // Pool still serves clean requests afterwards.
    b = agent.request(B, srv.origin(), get("/echo", "after"));
    CHECK(b.status == PoolStatus::ok && body_of(b) == "id=after;m=GET;p=/echo;b=0");
    CHECK(pool.stats().leased == 0);
}

static void test_request_injection_rejected() {
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    const PoolOrigin o{1, INADDR_LOOPBACK, 1};
    auto bad = [&](HttpRequestSpec s) { CHECK_STATUS(agent.request(A, o, s).status, PoolStatus::bad_request); };
    HttpRequestSpec s = get("/x", "1");
    s.target = "/x HTTP/1.1\r\nHost: h\r\n\r\nGET /pipelined";
    bad(s);
    s = get("/x", "1"); s.headers.push_back({"X-Evil", "v\r\n\r\nGET /pipelined HTTP/1.1"}); bad(s);
    s = get("/x", "1"); s.headers.push_back({"Content-Length", "0"}); bad(s);
    s = get("/x", "1"); s.headers.push_back({"transfer-encoding", "chunked"}); bad(s);
    s = get("/x", "1"); s.headers.push_back({"Connection", "upgrade"}); bad(s);
    s = get("/x", "1"); s.headers.push_back({"Bad Name", "v"}); bad(s);
    s = get("/x", "1"); s.headers.push_back({"X-Nul", std::string("a\0b", 3)}); bad(s);
    s = get("/x", "1"); s.method = "CONNECT"; bad(s);
    s = get("/x", "1"); s.host = "h\r\nX: y"; bad(s);
    s = get("/x", "1"); s.method = "HEAD"; s.body = {'x'}; bad(s);
    CHECK(pool.stats().connects == 0);  // nothing reached the wire
}

static void test_stale_keepalive_retry_and_revive() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    CHECK(agent.request(A, srv.origin(), get("/echo", "1")).pooled_after);
    srv.drop_all();  // server closes the idle keep-alive connection
    auto r = agent.request(B, srv.origin(), get("/echo", "2"));
    CHECK_STATUS(r.status, PoolStatus::ok);
    CHECK(body_of(r) == "id=2;m=GET;p=/echo;b=0");
    CHECK(srv.accepted() == 2);
    // Non-idempotent requests are never replayed.
    HttpRequestSpec post = get("/echo", "p");
    post.method = "POST";
    CHECK(agent.request(A, srv.origin(), get("/echo", "3")).pooled_after);
    srv.drop_all();
    // The probe usually catches the FIN; either way the POST must run at most once.
    r = agent.request(A, srv.origin(), post);
    CHECK(r.attempts == 1);
}

static void test_tenant_and_limits() {
    LoopbackServer srv;
    PoolConfig cfg;
    cfg.max_per_origin = 2;
    cfg.max_per_actor = 1;
    cfg.max_connections = 3;
    ActorSharedTcpPool pool(cfg);
    ActorHttpAgent agent(pool);
    CHECK(agent.request(A, srv.origin(1), get("/echo", "t1")).pooled_after);
    // Same host:port, different tenant => separate physical connection.
    auto r = agent.request(A, srv.origin(2), get("/echo", "t2"));
    CHECK(r.status == PoolStatus::ok && !r.reused_connection);
    CHECK(srv.accepted() == 2);
    auto a = pool.acquire(A, srv.origin(1));
    CHECK(a.status == PoolStatus::ok && a.lease.reused);
    CHECK_STATUS(pool.acquire(A, srv.origin(1)).status, PoolStatus::limit_actor);
    auto b = pool.acquire(B, srv.origin(1));
    CHECK(b.status == PoolStatus::ok && !b.lease.reused);
    CHECK_STATUS(pool.acquire(PoolActor{303, 1}, srv.origin(1)).status, PoolStatus::limit_origin);
    // Pool full: third tenant evicts the idle tenant-2 connection.
    auto c = pool.acquire(PoolActor{303, 1}, srv.origin(3));
    CHECK_STATUS(c.status, PoolStatus::ok);
    CHECK_STATUS(pool.acquire(PoolActor{404, 1}, srv.origin(4)).status, PoolStatus::pool_full);
    for (auto* p : {&a, &b, &c}) pool.release(p->lease.actor, p->lease, true);
    CHECK(pool.stats().idle == 3);
}

static void test_revoke_and_shutdown() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    CHECK(agent.request(B, srv.origin(), get("/echo", "b")).pooled_after);
    auto a = pool.acquire(A, srv.origin());
    auto b2 = pool.acquire(B, srv.origin());
    CHECK(a.status == PoolStatus::ok && b2.status == PoolStatus::ok);
    CHECK(pool.revoke_actor(A) == 1);
    CHECK_STATUS(pool.release(A, a.lease, true), PoolStatus::invalid_lease);
    CHECK(pool.stats().leased == 1);  // B unaffected
    CHECK_STATUS(pool.release(B, b2.lease, true), PoolStatus::ok);

    // Revoke while A is blocked mid-exchange: A's call returns promptly with an
    // error, the FD is closed only after A's I/O ends, and B keeps working.
    std::atomic<bool> done{false};
    HttpAgentResult slow;
    std::thread actor_a([&] { slow = agent.request(A, srv.origin(), get("/slow", "s")); done = true; });
    std::this_thread::sleep_for(50ms);
    CHECK(pool.revoke_actor(A) == 1);
    const auto t0 = std::chrono::steady_clock::now();
    actor_a.join();
    CHECK(std::chrono::steady_clock::now() - t0 < 300ms);
    CHECK(slow.status != PoolStatus::ok && !slow.pooled_after);
    auto rb = agent.request(B, srv.origin(), get("/echo", "b3"));
    CHECK(rb.status == PoolStatus::ok && body_of(rb) == "id=b3;m=GET;p=/echo;b=0");
    CHECK(pool.stats().leased == 0 && pool.stats().closing == 0);

    pool.shutdown();
    CHECK_STATUS(agent.request(B, srv.origin(), get("/echo", "x")).status, PoolStatus::shut_down);
    CHECK(pool.stats().idle == 0);
}

// Many actor threads share one pool; every response must carry the caller's
// own request id (no cross-talk), and nothing leaks.
static void test_concurrent_actors_no_crosstalk() {
    const int fds_before = open_fd_count();
    {
        LoopbackServer srv;
        PoolConfig cfg;
        cfg.max_per_origin = 4;
        cfg.max_per_actor = 1;
        ActorSharedTcpPool pool(cfg);
        ActorHttpAgent agent(pool);
        constexpr int kActors = 8, kRequests = 150;
        std::atomic<int> mismatches{0}, oks{0};
        std::vector<std::thread> actors;
        for (int t = 0; t < kActors; ++t) {
            actors.emplace_back([&, t] {
                const PoolActor me{1000u + static_cast<unsigned>(t), 1};
                for (int i = 0; i < kRequests; ++i) {
                    const std::string id = std::to_string(t) + "-" + std::to_string(i);
                    const char* path = (i % 13 == 0) ? "/double" : (i % 7 == 0) ? "/chunked" : (i % 11 == 0) ? "/close" : "/echo";
                    for (;;) {
                        auto r = agent.request(me, srv.origin(), get(path, id));
                        if (r.status == PoolStatus::limit_origin) { std::this_thread::yield(); continue; }
                        if (r.status != PoolStatus::ok) { ++mismatches; break; }
                        if (body_of(r) != "id=" + id + ";m=GET;p=" + path + ";b=0") ++mismatches;
                        ++oks;
                        break;
                    }
                }
            });
        }
        std::thread revoker([&] {  // churn: revoke an unrelated actor id repeatedly
            for (int i = 0; i < 200; ++i) { pool.revoke_actor(PoolActor{999, 1}); std::this_thread::yield(); }
        });
        for (auto& t : actors) t.join();
        revoker.join();
        CHECK(mismatches.load() == 0);
        CHECK(oks.load() == kActors * kRequests);
        const auto st = pool.stats();
        CHECK(st.leased == 0 && st.closing == 0 && st.idle <= 4);
        CHECK(st.reuses > 0);
        std::printf("  concurrent: connects=%llu reuses=%llu probe_discards=%llu unclean=%llu\n",
                    (unsigned long long)st.connects, (unsigned long long)st.reuses,
                    (unsigned long long)st.discarded_on_probe, (unsigned long long)st.closed_unclean);
    }
    std::this_thread::sleep_for(20ms);
    CHECK(open_fd_count() == fds_before);
}

// Revoke races with in-flight exchanges on many threads.
static void test_revoke_race_stress() {
    LoopbackServer srv;
    ActorSharedTcpPool pool({});
    ActorHttpAgent agent(pool);
    std::atomic<bool> stop{false};
    std::atomic<int> bad_body{0};
    std::vector<std::thread> actors;
    for (int t = 0; t < 4; ++t) {
        actors.emplace_back([&, t] {
            std::uint64_t inc = 1;
            int i = 0;
            while (!stop.load()) {
                const PoolActor me{50u + static_cast<unsigned>(t), inc};
                const std::string id = std::to_string(t) + ":" + std::to_string(i++);
                auto r = agent.request(me, srv.origin(), get("/echo", id));
                if (r.status == PoolStatus::ok && body_of(r) != "id=" + id + ";m=GET;p=/echo;b=0") ++bad_body;
                if (r.status != PoolStatus::ok) ++inc;  // "restart" after revocation
            }
        });
    }
    std::thread revoker([&] {
        for (int i = 0; i < 400; ++i)
            for (unsigned t = 0; t < 4; ++t)
                for (std::uint64_t inc = 1; inc < 8; ++inc) pool.revoke_actor(PoolActor{50u + t, inc});
    });
    revoker.join();
    stop = true;
    for (auto& t : actors) t.join();
    CHECK(bad_body.load() == 0);
    const auto st = pool.stats();
    CHECK(st.leased == 0 && st.closing == 0);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    struct { const char* name; void (*fn)(); } tests[] = {
        {"sequential_reuse_across_actors", test_sequential_reuse_across_actors},
        {"lease_is_bound_to_actor_and_generation", test_lease_is_bound_to_actor_and_generation},
        {"unsafe_connections_are_never_pooled", test_unsafe_connections_are_never_pooled},
        {"request_injection_rejected", test_request_injection_rejected},
        {"stale_keepalive_retry_and_revive", test_stale_keepalive_retry_and_revive},
        {"tenant_and_limits", test_tenant_and_limits},
        {"revoke_and_shutdown", test_revoke_and_shutdown},
        {"concurrent_actors_no_crosstalk", test_concurrent_actors_no_crosstalk},
        {"revoke_race_stress", test_revoke_race_stress},
    };
    for (auto& t : tests) {
        const int before = failures;
        t.fn();
        std::printf("%s %s\n", failures == before ? "PASS" : "FAIL", t.name);
    }
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("ACTOR_SHARED_TCP_POOL_ALL_PASS\n");
    return 0;
}
