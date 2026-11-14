#include "oreslang/runtime/NativeHttpExchange.hpp"
#include <stdexcept>
#include <thread>
using oreslang::runtime::NativeHttpExchange;
#define CHECK(x) do { if (!(x)) throw std::runtime_error(#x); } while (false)
int main() {
    NativeHttpExchange exchange;
    CHECK(!exchange.enqueue(0));
    CHECK(exchange.enqueue(7));
    auto first = exchange.token();
    CHECK(!exchange.enqueue(8));
    CHECK(!exchange.claim({first.generation, 8}));
    CHECK(exchange.rollback(first));
    CHECK(exchange.enqueue(7));
    auto second = exchange.token();
    CHECK(!exchange.claim(first));
    CHECK(exchange.claim(second));
    CHECK(!exchange.claim(second));
    CHECK(!exchange.reclaim(second));
    CHECK(exchange.complete(second));
    CHECK(!exchange.complete(second));
    CHECK(exchange.reclaim(second));
    int request = 0, response = 0;
    for (int n = 1; n <= 1000; ++n) {
        request = n;
        CHECK(exchange.enqueue(9));
        auto token = exchange.token();
        std::thread worker([&] {
            CHECK(exchange.claim(token));
            response = request * 2;
            CHECK(exchange.complete(token));
        });
        while (exchange.phase() != NativeHttpExchange::Phase::completed) std::this_thread::yield();
        CHECK(exchange.reclaim(token));
        CHECK(response == n * 2);
        worker.join();
    }
}
