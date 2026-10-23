#include "oreslang/runtime/RequestSlotTable.hpp"
#include "oreslang/runtime/HttpRequestHeadView.hpp"
#include <cassert>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>

static std::atomic<std::size_t> news{0};
void* operator new(std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t n) {
    news.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
    return 0;
}
