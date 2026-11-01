#include "oreslang/runtime/ActorIoEnvelope.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string_view>
#include <vector>

using oreslang::runtime::ActorIoAuthority;
using oreslang::runtime::NativeActorIoEnvelope;

template <class F>
static void expect_stale(F&& fn) {
    bool rejected = false;
    try {
        fn();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}

int main() {
    using Io = NativeActorIoEnvelope<4, 8, 32, 4, 16, 32>;
    Io io(ActorIoAuthority{41, 1, 1001});
    auto auth = io.authority();

    std::uint8_t input[] = {1, 2, 3};
    assert(io.try_feed_input(auth, input, sizeof(input)));
    input[0] = 9;
    std::uint8_t read[8] = {};
    std::size_t read_len = 0;
    assert(io.try_read_input(auth, read, sizeof(read), read_len));
    assert(read_len == 3 && read[0] == 1 && read[1] == 2 && read[2] == 3);

    const std::uint8_t output[] = {4, 5};
    assert(io.try_write_output(auth, output, sizeof(output)));
    assert(io.try_emit_error(auth, "E_PARSE", "bad token"));
    assert(io.try_emit_event(auth, "ready", "v1"));
    std::memset(read, 0, sizeof(read));
    assert(io.try_read_output(auth, read, sizeof(read), read_len));
    assert(read_len == 2 && read[0] == 4 && read[1] == 5);

    std::string_view first;
    std::string_view second;
    assert(io.try_read_error(auth, first, second));
    assert(first == "E_PARSE" && second == "bad token");
    assert(io.try_read_event(auth, first, second));
    assert(first == "ready" && second == "v1");

    std::array<std::pair<std::string_view, std::string_view>, 4> metadata{};
    metadata[0] = {"trace_id", "abc"};
    metadata[1] = {"route", "/v1"};
    assert(io.replace_metadata(auth, metadata, 2));
    assert(io.metadata_version(auth) == 2);
    assert(io.metadata_value(auth, "trace_id").value() == "abc");
    assert(io.metadata_value(auth, "route").value() == "/v1");

    // Output backpressure is isolated from events.
    const std::uint8_t full[] = {0,1,2,3,4,5,6,7};
    for (int i = 0; i < 4; ++i)
        assert(io.try_write_output(auth, full, sizeof(full)));
    assert(!io.try_write_output(auth, full, sizeof(full)));
    assert(io.try_emit_event(auth, "progress", "still-live"));

    const auto stale = auth;
    const auto fresh = io.restart(2, 1002);
    expect_stale([&] { (void)io.metadata_version(stale); });
    expect_stale([&] { (void)io.try_write_output(stale, output, sizeof(output)); });
    assert(io.try_write_output(fresh, output, sizeof(output)));

    auto forged = fresh;
    forged.nonce = 9999;
    expect_stale([&] { (void)io.try_feed_input(forged, input, sizeof(input)); });

    // 128 actor envelopes preserve independent FIFO state with no shared globals.
    std::vector<NativeActorIoEnvelope<2, 2, 8, 1, 4, 4>> actors;
    actors.reserve(128);
    for (std::uint64_t i = 0; i < 128; ++i)
        actors.emplace_back(ActorIoAuthority{1000 + i, 1, 5000 + i});
    for (std::size_t i = 0; i < actors.size(); ++i) {
        auto a = actors[i].authority();
        const std::uint8_t one[] = {
            static_cast<std::uint8_t>(i), 1
        };
        const std::uint8_t two[] = {
            static_cast<std::uint8_t>(i), 2
        };
        assert(actors[i].try_write_output(a, one, sizeof(one)));
        assert(actors[i].try_write_output(a, two, sizeof(two)));
    }
    for (std::size_t i = actors.size(); i-- > 0;) {
        auto a = actors[i].authority();
        std::uint8_t bytes[2] = {};
        assert(actors[i].try_read_output(a, bytes, sizeof(bytes), read_len));
        assert(bytes[0] == static_cast<std::uint8_t>(i) && bytes[1] == 1);
        assert(actors[i].try_read_output(a, bytes, sizeof(bytes), read_len));
        assert(bytes[0] == static_cast<std::uint8_t>(i) && bytes[1] == 2);
    }

    return 0;
}
