// Real production BoundedActorPort, preallocated slots and shared ledger.
// This is HOST runtime evidence only, not compiler-backed .atto guest AOT.
#include "oreslang/runtime/BoundedActorPort.hpp"
#include "oreslang/runtime/ActorMailboxCompletion.hpp"
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>
using oreslang::runtime::ActorMailboxCompletion;
using oreslang::runtime::BoundedActorPort;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("physical port reference oracle: " #x); } while(false)
struct MoveOnly {
    std::unique_ptr<int> payload;
    explicit MoveOnly(int n) : payload(std::make_unique<int>(n)) {}
    MoveOnly(MoveOnly&&) noexcept = default;
    MoveOnly(const MoveOnly&) = delete;
};

int main() {
    auto ledger = std::make_shared<ActorMailboxCompletion>(12);
    BoundedActorPort<MoveOnly> port(8, 80, ledger);
    using Status = BoundedActorPort<MoveOnly>::SelectiveStatus;
    std::deque<std::pair<int, std::uint32_t>> oracle;
    std::mt19937 rng(20261110);
    int next = 1;
    for (int step = 0; step < 30000; ++step) {
        unsigned op = rng() % 4;
        if (op < 2) {
            const int value = next;
            const std::uint32_t tag = 1 + rng() % 5;
            MoveOnly owned(value);
            const bool admitted = port.offer(std::move(owned), 8, tag);
            if (admitted) {
                CHECK(!owned.payload);
                oracle.emplace_back(value, tag);
                ++next;
            } else {
                CHECK(owned.payload && *owned.payload == value);
            }
        } else if (op == 2) {
            const std::uint32_t tag = 1 + rng() % 5;
            auto expected = oracle.end();
            for (auto it = oracle.begin(); it != oracle.end(); ++it)
                if (it->second == tag) { expected = it; break; }
            auto selected = port.try_receive_tagged(tag, 100);
            CHECK(selected.status != Status::Continue);
            if (expected == oracle.end()) {
                CHECK(selected.status == Status::NoMatch && !selected.delivery);
            } else {
                CHECK(selected.status == Status::Matched && selected.delivery);
                CHECK(*selected.delivery->value().payload == expected->first);
                CHECK(selected.delivery->acknowledge_released());
                ledger->complete(selected.delivery->sequence());
                oracle.erase(expected);
            }
        } else {
            auto selected = port.try_receive();
            CHECK(bool(selected) == !oracle.empty());
            if (selected) {
                CHECK(*selected->value().payload == oracle.front().first);
                CHECK(selected->acknowledge_released());
                ledger->complete(selected->sequence());
                oracle.pop_front();
            }
        }
        CHECK(port.used_frames() == oracle.size());
        CHECK(port.used_bytes() == oracle.size() * 8);
    }
    while (auto delivered = port.try_receive()) {
        CHECK(!oracle.empty());
        CHECK(*delivered->value().payload == oracle.front().first);
        CHECK(delivered->acknowledge_released());
        ledger->complete(delivered->sequence());
        oracle.pop_front();
    }
    CHECK(oracle.empty() && port.used_frames() == 0);
    CHECK(ledger->completed_through(ledger->snapshot()));
    port.close();
}
