#include "oreslang/runtime/ActorStateReadWriteGate.hpp"

#include <stdexcept>
#include <thread>
#include <vector>

using oreslang::runtime::ActorStateReadWriteGate;
using Gate = ActorStateReadWriteGate;
#define CHECK(x) do { if (!(x)) throw std::runtime_error("failed: " #x); } while (false)

int main() {
    Gate gate;
    const auto a = gate.request(Gate::Mode::Read);
    const auto b = gate.request(Gate::Mode::Read);
    CHECK(a.admission == Gate::Admission::acquired);
    CHECK(b.admission == Gate::Admission::acquired);
    CHECK(gate.active_readers() == 2);

    const auto w = gate.request(Gate::Mode::Write);
    const auto c = gate.request(Gate::Mode::Read);
    CHECK(w.admission == Gate::Admission::queued);
    CHECK(c.admission == Gate::Admission::queued);
    CHECK(gate.waiting() == 2);
    CHECK(!gate.holds(w.token, Gate::Mode::Write));
    CHECK(gate.release(a.token).empty());
    CHECK(gate.active_readers() == 1);

    auto awakened = gate.release(b.token);
    CHECK(awakened.size() == 1 && awakened[0] == w.token);
    CHECK(gate.holds(w.token, Gate::Mode::Write));
    CHECK(!gate.holds(w.token, Gate::Mode::Read));
    CHECK(gate.active_readers() == 0);
    CHECK(gate.release(a.token).empty()); // double completion
    awakened = gate.release(w.token);
    CHECK(awakened.size() == 1 && awakened[0] == c.token);
    CHECK(gate.holds(c.token, Gate::Mode::Read));
    CHECK(gate.release(c.token).empty());

    // Writer cancellation unblocks queued readers even with an active reader.
    const auto keep = gate.request(Gate::Mode::Read);
    const auto cancelled = gate.request(Gate::Mode::Write);
    const auto queued1 = gate.request(Gate::Mode::Read);
    const auto queued2 = gate.request(Gate::Mode::Read);
    awakened = gate.release(cancelled.token);
    CHECK(awakened.size() == 2);
    CHECK(awakened[0] == queued1.token && awakened[1] == queued2.token);
    CHECK(gate.active_readers() == 3);
    CHECK(gate.release(cancelled.token).empty());
    CHECK(gate.release(keep.token).empty());
    CHECK(gate.release(queued1.token).empty());
    CHECK(gate.release(queued2.token).empty());

    // Writer FIFO priority: arriving readers never overtake a queued writer.
    {
        Gate ordered;
        auto first = ordered.request(Gate::Mode::Read);
        auto writer = ordered.request(Gate::Mode::Write);
        for (int i = 0; i < 128; ++i) {
            auto later = ordered.request(Gate::Mode::Read);
            CHECK(later.admission == Gate::Admission::queued);
        }
        auto admitted = ordered.release(first.token);
        CHECK(admitted.size() == 1 && admitted[0] == writer.token);
        CHECK(ordered.holds(writer.token, Gate::Mode::Write));
        admitted = ordered.release(writer.token);
        CHECK(admitted.size() == 128);
        CHECK(ordered.active_readers() == 128);
        auto all = ordered.retire();
        CHECK(all.size() == 128);
        CHECK(!ordered.holds(admitted.front(), Gate::Mode::Read));
    }

    // Distinct gate scopes cannot release one another's tickets.
    Gate other;
    const auto foreign = other.request(Gate::Mode::Write);
    bool rejected = false;
    try { (void)gate.release(foreign.token); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK(other.holds(foreign.token, Gate::Mode::Write));
    CHECK(other.release(foreign.token).empty());

    // Readers are admitted without a guest-visible OS-lock wait.
    std::vector<std::thread> threads;
    std::vector<Gate::Request> reads(16);
    for (std::size_t i = 0; i < reads.size(); ++i) {
        threads.emplace_back([&gate, &reads, i] {
            reads[i] = gate.request(Gate::Mode::Read);
        });
    }
    for (auto& thread : threads) thread.join();
    CHECK(gate.active_readers() == reads.size());
    for (const auto& request : reads) {
        CHECK(request.admission == Gate::Admission::acquired);
        CHECK(gate.release(request.token).empty());
    }

    const auto terminal = gate.request(Gate::Mode::Read);
    const auto terminal_writer = gate.request(Gate::Mode::Write);
    const auto terminal_reader = gate.request(Gate::Mode::Read);
    const auto revoked = gate.retire();
    CHECK(revoked.size() == 3);
    CHECK(revoked[0] == terminal.token);
    CHECK(revoked[1] == terminal_writer.token);
    CHECK(revoked[2] == terminal_reader.token);
    CHECK(gate.retire().empty()); // idempotent
    CHECK(!gate.holds(terminal.token, Gate::Mode::Read));
    CHECK(gate.release(terminal.token).empty());
    CHECK(gate.release(terminal_writer.token).empty());
    rejected = false;
    try { (void)gate.request(Gate::Mode::Read); }
    catch (const std::logic_error&) { rejected = true; }
    CHECK(rejected);
    return 0;
}
