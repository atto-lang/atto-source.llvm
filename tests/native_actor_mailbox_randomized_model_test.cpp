// Deterministic model-based test against production ActorMailboxCompletion.
// Independent model stores every published frame rather than using ring slots.
#include "oreslang/runtime/ActorMailboxCompletion.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

using oreslang::runtime::ActorMailboxCompletion;

struct Frame { bool complete = false; bool aborted = false; };

static void require(bool good, const char *why, unsigned window, std::size_t step) {
    if (!good) {
        std::cerr << "FAIL window=" << window << " step=" << step << " " << why << '\n';
        std::abort();
    }
}

static void verify(unsigned window, std::uint32_t seed, std::size_t iterations) {
    ActorMailboxCompletion ledger(window);
    std::minstd_rand rng(seed);
    std::vector<Frame> records(1); // sequence 0 is never published
    std::uint64_t watermark = 0;
    std::uint64_t first_aborted = 0;
    std::size_t preparing = 0;

    for (std::size_t step = 0; step != iterations; ++step) {
        const auto snapshot = records.size() - 1;
        const auto outstanding = snapshot - watermark;
        const auto op = rng() % 7;
        if (op == 0 || op == 1) {
            const bool expected = outstanding + preparing < window;
            const bool observed = ledger.reserve_prepare();
            require(observed == expected, "reserve capacity", window, step);
            if (observed) ++preparing;
        } else if (op == 2 && preparing) {
            ledger.cancel_prepare();
            --preparing;
        } else if (op == 3 && preparing) {
            const std::uint64_t seq = records.size();
            const auto observed = ledger.publish([&](std::uint64_t got) {
                require(got == seq, "publication sequence", window, step);
            });
            require(observed == seq, "published receipt sequence", window, step);
            records.emplace_back();
            --preparing;
        } else if (op == 4 && preparing) {
            const auto prev_snapshot = ledger.snapshot();
            bool failed = false;
            try {
                ledger.publish([](std::uint64_t) {
                    throw std::runtime_error("synthetic queue allocation failure");
                });
            } catch (const std::runtime_error&) { failed = true; }
            require(failed && ledger.snapshot() == prev_snapshot,
                    "failed publication leaves reservation and sequence untouched",
                    window, step);
        } else if (op == 5 && outstanding) {
            // Complete a random unfinished frame from the bounded active window.
            std::vector<std::uint64_t> candidates;
            for (auto seq = watermark + 1; seq <= snapshot; ++seq) {
                if (!records[seq].complete) candidates.push_back(seq);
            }
            if (!candidates.empty()) {
                const auto seq = candidates[rng() % candidates.size()];
                const bool aborted = (rng() % 23) == 0;
                ledger.complete(seq, aborted);
                records[seq].complete = true;
                records[seq].aborted = aborted;
                if (aborted && !first_aborted) first_aborted = seq;
                else if (aborted) first_aborted = std::min(first_aborted, seq);
                while (watermark + 1 < records.size() &&
                       records[watermark + 1].complete) ++watermark;
            }
        }
        require(ledger.snapshot() == records.size() - 1,
                "snapshot matches independent publication count", window, step);
        require(ledger.watermark() == watermark,
                "contiguous watermark matches independent model", window, step);
        if (watermark) {
            const auto expected = first_aborted && first_aborted <= watermark
                ? ActorMailboxCompletion::DrainStatus::Aborted
                : ActorMailboxCompletion::DrainStatus::Delivered;
            require(ledger.drain_status(watermark) == expected,
                    "completed drain distinguishes delivered from aborted", window, step);
            require(ledger.completed_through(watermark) ==
                    (expected == ActorMailboxCompletion::DrainStatus::Delivered),
                    "positive drain predicate cannot certify aborted work", window, step);
        }
        if (watermark < records.size() - 1) {
            require(ledger.drain_status(watermark + 1) ==
                        ActorMailboxCompletion::DrainStatus::Pending,
                    "incomplete drain must remain pending", window, step);
        }
    }
    std::cout << "PASS window=" << window << " seed=" << seed
              << " operations=" << iterations << " committed=" << records.size() - 1
              << " watermark=" << watermark << '\n';
}

int main() {
    for (unsigned window : {1U, 2U, 3U, 4U, 7U, 31U, 64U}) {
        for (std::uint32_t seed : {1U, 17U, 991U, 65537U}) {
            verify(window, seed + window * 7919U, 10000);
        }
    }
    std::cout << "PASS mailbox_randomized_model 280000_operations\n";
}
