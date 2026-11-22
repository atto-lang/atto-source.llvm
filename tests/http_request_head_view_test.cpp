#include "oreslang/runtime/RequestSlotTable.hpp"
#include "oreslang/runtime/HttpRequestHeadView.hpp"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>

static std::atomic<std::size_t> news{0};
__attribute__((noinline)) void* operator new(std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void* operator new[](std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
__attribute__((noinline)) void operator delete(void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete(void* p, std::size_t) noexcept { std::free(p); }
__attribute__((noinline)) void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

using namespace oreslang::runtime;
int main() {
    using Pool = RequestSlotTable<1, 4096>;
    Pool pool;
    constexpr char valid[] =
        "POST /submit HTTP/1.1\r\nHost: example.test\r\nContent-Type: text/plain\r\n\r\nhello";
    const auto before = news.load(std::memory_order_relaxed);
    for (int i = 0; i < 10000; ++i) {
        auto in = pool.begin(1);
        assert(in);
        std::memcpy(in->data(), valid, sizeof(valid) - 1);
        auto token = in->publish(sizeof(valid) - 1);
        auto moved = pool.transfer(token, 1, 2);
        assert(moved && !pool.borrow(token, 1));
        auto leased = pool.borrow(*moved, 2);
        assert(leased);
        auto parsed = HttpRequestHeadView::parse(leased->view());
        assert(parsed && parsed->method() == "POST" && parsed->target() == "/submit");
        assert(parsed->version() == "HTTP/1.1" && parsed->header_count() == 2);
        assert(parsed->first_header("hOsT") == "example.test");
        assert(parsed->first_header("content-type") == "text/plain");
        assert(!parsed->first_header("missing"));
        assert(leased->view().substr(parsed->head_bytes()) == "hello");
        leased.reset();
        assert(pool.close(*moved, 2));
    }
    assert(news.load(std::memory_order_relaxed) == before);
    const std::string_view invalid[] = {
        "GET / HTTP/1.1\nHost: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n",
        "GET / HTTP/1.1\r\n Folded: x\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n X: y\r\n\r\n",
        "GET / HTTP/1.1\r\nHost : x\r\n\r\n",
        "GET / HTTP/1.1\r\nBad_Name: x\r\n\r\n",
        "GET / HTTP/1.1\r\nX: a\x01""b\r\n\r\n",
        "GET / HTTP/1.1\r\nX: a\x7f""b\r\n\r\n",
        "GET / HTTP/2.0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\nY: z\r\n\r\n"
    };
    for (auto v : invalid) assert(!HttpRequestHeadView::parse(v));
    assert(!HttpRequestHeadView::parse(""));
    assert(!HttpRequestHeadView::parse("GET / HTTP/1.1\r\n"));
    // This fast path is intentionally more restrictive than full HTTP/1.x:
    // only an entirely received, unframed GET/HEAD may be dispatched.
    auto eligible = HttpRequestHeadView::parse(
        "GET /safe HTTP/1.1\r\nHost: example.test\r\n\r\n");
    assert(eligible && eligible->safe_unframed_request());
    auto head = HttpRequestHeadView::parse(
        "HEAD / HTTP/1.0\r\n\r\n");
    assert(head && head->safe_unframed_request());
    auto lengthZero = HttpRequestHeadView::parse(
        "GET / HTTP/1.1\r\nHost: example.test\r\nContent-Length: 0\r\n\r\n");
    assert(lengthZero && lengthZero->safe_unframed_request());
    const std::string_view forbidden[] = {
        "GET / HTTP/1.1\r\n\r\n", // Host mandatory for HTTP/1.1
        "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",
        "GET / HTTP/1.1\r\nHost:\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: example.test,evil.test\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: user@example.test\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: example.test/admin\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 00\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\nContent-Length: 0\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nExpect: 100-continue\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: y\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\n\r\nbody",
        "POST / HTTP/1.1\r\nHost: x\r\n\r\n"
    };
    for (const auto input : forbidden) {
        const auto parsed = HttpRequestHeadView::parse(input);
        assert(parsed && !parsed->safe_unframed_request());
    }
    // A valid prefix at exactly the 32KiB metadata boundary must not cause
    // the helper to ignore trailing supplied bytes.
    std::string padded = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    padded.append(HttpRequestHeadView::max_head_bytes - padded.size(), 'z');
    assert(!HttpRequestHeadView::parse(padded)->safe_unframed_request());
    return 0;
}
