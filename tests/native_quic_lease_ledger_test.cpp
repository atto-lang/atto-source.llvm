#include "oreslang/runtime/NativeQuicLeaseLedger.hpp"
#include <cassert>
#include <iostream>

using namespace oreslang::native_quic;
using Ledger = NativeQuicLeaseLedger<2, 3, 2, 1>;

int main() {
    Ledger invalid({0, 1});
    assert(!invalid.admit_connection().has_value());
    Ledger stale({5, 0});
    assert(!stale.admit_connection().has_value());

    Ledger ledger({42, 7});
    auto c1 = ledger.admit_connection();
    auto c2 = ledger.admit_connection();
    assert(c1 && c2 && c1->ticket != c2->ticket);
    assert(!ledger.admit_connection());
    assert(ledger.active_connections() == 2);
    auto s1 = ledger.admit_stream(*c1, Ledger::Direction::Bidirectional);
    auto s2 = ledger.admit_stream(*c1, Ledger::Direction::Bidirectional);
    assert(s1 && s2);
    assert(!ledger.admit_stream(*c1, Ledger::Direction::Bidirectional));
    auto s3 = ledger.admit_stream(*c1, Ledger::Direction::Unidirectional);
    assert(s3);
    assert(!ledger.admit_stream(*c1, Ledger::Direction::Unidirectional));
    assert(!ledger.admit_stream(*c1, static_cast<Ledger::Direction>(255)));
    assert(!ledger.admit_stream(*c2, Ledger::Direction::Bidirectional));
    assert(ledger.active_streams() == 3);

    auto forged = *s1;
    ++forged.actor_id;
    assert(!ledger.settle_stream(forged));
    assert(!ledger.settle_connection(*c1));
    assert(ledger.begin_close(*c1));
    assert(!ledger.admit_stream(*c1, Ledger::Direction::Bidirectional));
    assert(!ledger.settle_connection(*c1));

    // Cancellation/drain does not free one byte of quota by itself.
    ledger.begin_actor_retirement();
    assert(!ledger.admit_connection());
    assert(!ledger.admit_stream(*c2, Ledger::Direction::Bidirectional));
    assert(!ledger.fully_retired());
    assert(ledger.active_streams() == 3);

    assert(ledger.settle_stream(*s1));
    assert(!ledger.settle_stream(*s1)); // exactly once
    assert(ledger.settle_stream(*s2));
    assert(ledger.settle_stream(*s3));
    assert(ledger.active_streams() == 0);
    assert(ledger.settle_connection(*c1));
    assert(!ledger.settle_connection(*c1));
    assert(ledger.settle_connection(*c2));
    assert(ledger.active_connections() == 0);
    assert(ledger.fully_retired());
    assert(!ledger.admit_connection()); // no re-admission after retirement

    // Recycle physical slots many times. Tickets must never be reused, and
    // late close/stream callbacks for the previous incarnation must fail.
    Ledger reuse({91, 4});
    std::uint64_t last_ticket = 0;
    for (int i = 0; i < 10000; ++i) {
        auto conn = reuse.admit_connection();
        assert(conn && conn->ticket > last_ticket);
        last_ticket = conn->ticket;
        auto old_stream = reuse.admit_stream(*conn, Ledger::Direction::Bidirectional);
        assert(old_stream);
        assert(reuse.settle_stream(*old_stream));
        auto newer_stream = reuse.admit_stream(*conn, Ledger::Direction::Bidirectional);
        assert(newer_stream && newer_stream->ticket != old_stream->ticket);
        assert(!reuse.settle_stream(*old_stream));
        assert(reuse.settle_stream(*newer_stream));
        assert(reuse.begin_close(*conn));
        assert(reuse.settle_connection(*conn));
        assert(!reuse.begin_close(*conn));
        assert(!reuse.settle_connection(*conn));
    }
    assert(reuse.active_connections() == 0);
    assert(reuse.active_streams() == 0);
    reuse.begin_actor_retirement();
    assert(reuse.fully_retired());
    std::cout << "QUIC native host lease ledger: bounded retirement checks passed\n";
}
