#include "oreslang/runtime/BoundedActorPort.hpp"
#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>

#define CHECK(x) do { if (!(x)) throw std::runtime_error("bounded selective slot oracle: " #x); } while(false)

using Port = oreslang::runtime::BoundedActorPort<int>;
int main() {
    Port port(32, 256);
    std::deque<std::pair<std::uint32_t,int>> expected;
    std::minstd_rand rng(20261110);
    int next_value = 1;
    std::uint64_t last_sequence = 0;
    for (int turn = 0; turn < 100000; ++turn) {
        auto op = rng() % 4;
        if (op <= 1) {
            auto tag = std::uint32_t(1 + rng() % 7);
            auto value = std::make_unique<int>(next_value);
            bool accepted = port.offer_owned(value, 8, tag);
            CHECK(accepted == (expected.size() < 32));
            if (accepted) {
                CHECK(!value);
                expected.emplace_back(tag, next_value++);
            } else CHECK(value && *value == next_value);
        } else if (op == 2) {
            const auto tag = std::uint32_t(1 + rng() % 7);
            auto matching = std::find_if(expected.begin(), expected.end(),
                [&](const auto& item) { return item.first == tag; });
            const bool should_match = matching != expected.end();
            Port::SelectiveCursor cursor;
            bool done = false;
            for (int i = 0; i <= 32 && !done; ++i) {
                auto selected = port.try_receive_tagged(tag, 1, cursor);
                CHECK(selected.inspected <= 1);
                if (selected.status == Port::SelectiveStatus::Continue) {
                    CHECK(!selected.delivery);
                    cursor = std::move(selected.cursor);
                    continue;
                }
                done = true;
                if (should_match) {
                    CHECK(selected.status == Port::SelectiveStatus::Matched && selected.delivery);
                    CHECK(selected.delivery->value() == matching->second);
                    CHECK(selected.delivery->sequence() > 0);
                    CHECK(selected.delivery->acknowledge_released());
                    CHECK(!selected.delivery->acknowledge_released());
                    expected.erase(matching);
                } else CHECK(selected.status == Port::SelectiveStatus::NoMatch && !selected.delivery);
            }
            CHECK(done);
        } else {
            auto receipt = port.try_receive();
            CHECK(bool(receipt) == !expected.empty());
            if (receipt) {
                CHECK(receipt->value() == expected.front().second);
                CHECK(receipt->sequence() > last_sequence);
                last_sequence = receipt->sequence();
                CHECK(receipt->acknowledge_released());
                expected.pop_front();
            }
        }
        CHECK(port.used_frames() == expected.size());
        CHECK(port.used_bytes() == expected.size() * 8);
    }
    while (auto receipt = port.try_receive()) {
        CHECK(!expected.empty());
        CHECK(receipt->value() == expected.front().second);
        CHECK(receipt->acknowledge_released());
        expected.pop_front();
    }
    CHECK(expected.empty() && port.used_frames() == 0 && port.used_bytes() == 0);
    port.close();
    Port sealed(1, 8);
    auto owner = std::make_unique<int>(9);
    CHECK(sealed.offer_owned(owner, 8, 7));
    bool refused_close = false;
    try { sealed.close(); } catch (const std::logic_error&) { refused_close = true; }
    CHECK(refused_close);
    auto pending = sealed.try_receive_tagged(7, 1);
    CHECK(pending.status == Port::SelectiveStatus::Matched && pending.delivery);
    CHECK(pending.delivery->value() == 9 && pending.delivery->acknowledge_released());
    sealed.close();
}
