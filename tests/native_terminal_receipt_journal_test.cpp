#include "oreslang/runtime/NativeTerminalReceiptJournal.hpp"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using namespace oreslang::runtime;

static void check(bool condition, const char* error) {
    if (!condition) throw std::runtime_error(error);
}
template <typename Fn>
static void rejects(Fn action, const char* error) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    check(rejected, error);
}
static DurableTerminalKey key(std::uint64_t logical = 71) {
    return DurableTerminalKey::from(88, 901, 901, 0, 1,
                                    901, 1, 5, 1, 440, 750,
                                    logical, 2);
}
static ExternalEffectProof proof(DurableTerminalKey k, std::uint64_t nonce) {
    ExternalEffectProof p{k, nonce, {}};
    for (std::size_t i = 0; i < p.evidence_sha256.size(); ++i)
        p.evidence_sha256[i] = static_cast<std::uint8_t>(i + 1);
    return p;
}
static void run_crashed_child(const std::string& path, int scenario) {
    const pid_t pid = ::fork();
    check(pid >= 0, "fork failed");
    if (pid == 0) {
        try {
            NativeTerminalReceiptJournal journal(path);
            auto k = key(static_cast<std::uint64_t>(scenario));
            journal.record(k);
            if (!journal.record_physical_done(k, [] { return true; }))
                ::_exit(21);
            const auto nonce = journal.claim(k);
            if (scenario == 73) {
                auto p = proof(k, nonce);
                if (!journal.prove_external_effect(p, [](const auto&) { return true; }))
                    ::_exit(22);
            }
            // Simulate SIGKILL-style exit with no C++ destructors. fsync
            // happened on every committed transition.
            ::_exit(0);
        } catch (...) { ::_exit(23); }
    }
    int status = 0;
    check(::waitpid(pid, &status, 0) == pid && WIFEXITED(status)
              && WEXITSTATUS(status) == 0, "crashed child did not persist fixture");
}
int main() {
    char tmp[] = "/tmp/oreslang-terminal-receipt-XXXXXX";
    const char* dir = ::mkdtemp(tmp);
    check(dir != nullptr, "mkdtemp failed");
    const std::string path = std::string(dir) + "/receipts.journal";

    const auto k = key();
    {
        NativeTerminalReceiptJournal journal(path);
        rejects([&] {
            (void)journal.record_physical_done(k, [] { return true; });
        }, "DONE was allowed without recorded terminal");
        journal.record(k);
        rejects([&] { journal.record(k); },
                "same exact terminal event recorded twice");
        check(!journal.record_physical_done(k, [] { return false; }),
              "unverified physical DONE published");
        check(journal.inspect(k).phase == DurableTerminalPhase::recorded,
              "unverified physical DONE changed journal");
        rejects([&] { (void)journal.claim(k); },
                "host claimed before physical DONE");
        check(journal.record_physical_done(k, [] { return true; }),
              "verified physical DONE not durably recorded");
        const auto nonce = journal.claim(k);
        check(nonce == 1 && journal.inspect(k).ambiguous(),
              "first claim did not become durable ambiguous");
        rejects([&] { (void)journal.claim(k); },
                "CLAIMED terminal effect was automatically replayed");
        auto p = proof(k, nonce);
        check(!journal.prove_external_effect(
                  p, [](const auto&) { return false; }),
              "false external proof advanced durable receipt");
        check(journal.inspect(k).ambiguous(),
              "false external proof removed quarantine");
        auto wrong = p;
        wrong.nonce++;
        check(!journal.prove_external_effect(
                  wrong, [](const auto&) { return true; }),
              "foreign claim nonce proved effect");
        rejects([&] { journal.acknowledge(p); },
                "CLAIMED terminal receipt acknowledged without effect proof");

        // Another writer in this process must not share lock ownership.
        rejects([&] { NativeTerminalReceiptJournal competing(path); },
                "simultaneous writer acquired same append journal");
        check(journal.prove_external_effect(
                  p, [&](const auto& receipt) {
                      return receipt.key == k && receipt.nonce == nonce
                          && receipt.evidence_sha256 == p.evidence_sha256;
                  }), "valid independent external effect proof not recorded");
        rejects([&] { journal.acknowledge(wrong); },
                "wrong proof nonce acknowledged exact effect");
        journal.acknowledge(p);
        rejects([&] { journal.acknowledge(p); }, "duplicate ACK succeeded");
        check(journal.inspect(k).phase == DurableTerminalPhase::acknowledged,
              "ACK not durable in running process");
        // The externally committed host effect was invoked exactly once;
        // journal recovery will never call this host effect on its own.
    }
    {
        NativeTerminalReceiptJournal reopened(path);
        check(reopened.inspect(k).phase == DurableTerminalPhase::acknowledged,
              "ACK did not survive close/reopen");
        rejects([&] { (void)reopened.claim(k); },
                "ACKNOWLEDGED effect was replayed after reopen");
    }

    // Crash after fsynced CLAIM, before any external evidence exists.
    // Recovery preserves ambiguity, NOT an implicit effect success/retry.
    run_crashed_child(path, 72);
    {
        NativeTerminalReceiptJournal reopened(path);
        auto crashed = key(72);
        check(reopened.inspect(crashed).ambiguous(),
              "post-crash claim was not quarantined");
        rejects([&] { (void)reopened.claim(crashed); },
                "post-crash ambiguous effect was automatically retried");
        const auto p = proof(crashed, reopened.inspect(crashed).nonce);
        check(!reopened.prove_external_effect(
                  p, [](const auto&) { return false; }),
              "missing independently committed effect proof was accepted");
        check(reopened.inspect(crashed).ambiguous(),
              "missing evidence lost quarantine after crash");
        check(reopened.prove_external_effect(
                  p, [](const auto&) { return true; }),
              "verified external ledger evidence could not resolve ambiguity");
        reopened.acknowledge(p);
    }

    // Crash after verified external proof but before ACK. Recovery can
    // acknowledge *without* re-running the external effect.
    run_crashed_child(path, 73);
    {
        NativeTerminalReceiptJournal reopened(path);
        const auto crashed = key(73);
        const auto state = reopened.inspect(crashed);
        check(state.phase == DurableTerminalPhase::effect_proven,
              "committed proof disappeared on crash");
        rejects([&] { (void)reopened.claim(crashed); },
                "post-proof restart attempted second external effect");
        reopened.acknowledge(proof(crashed, state.nonce));
        check(reopened.inspect(crashed).phase == DurableTerminalPhase::acknowledged,
              "post-proof ACK recovery failed");
    }

    // Two in-process claimants contend over one exact generation.
    {
        NativeTerminalReceiptJournal journal(path);
        const auto concurrent = key(74);
        journal.record(concurrent);
        journal.record_physical_done(concurrent, [] { return true; });
        std::atomic<int> claimed{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 8; ++i) {
            threads.emplace_back([&] {
                try { (void)journal.claim(concurrent); ++claimed; }
                catch (const std::logic_error&) {}
            });
        }
        for (auto& t : threads) t.join();
        check(claimed == 1 && journal.inspect(concurrent).ambiguous(),
              "multiple concurrent host effects claimed one terminal receipt");
    }

    // A hard link is a second filesystem name for the same inode. The
    // path-local writer lock and O_NOFOLLOW alone cannot prevent an external
    // writer from opening that alias, so reject multi-link journals on open.
    {
        const std::string alias = std::string(dir) + "/receipts.alias";
        check(::link(path.c_str(), alias.c_str()) == 0,
              "failed to create hard-link journal fixture");
        rejects([&] { NativeTerminalReceiptJournal bad(path); },
                "hard-linked receipt journal was admitted by original path");
        rejects([&] { NativeTerminalReceiptJournal bad(alias); },
                "hard-linked receipt journal was admitted by alternate path");
        check(::unlink(alias.c_str()) == 0,
              "failed to remove journal alias");
        NativeTerminalReceiptJournal recovered(path);
        check(recovered.inspect(k).phase == DurableTerminalPhase::acknowledged,
              "single-link journal did not reopen after alias removal");
    }

    // A torn last frame and silent record mutation both fail closed.
    {
        int fd = ::open(path.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
        check(fd >= 0, "could not open journal for deliberate corruption");
        const char torn[] = "OREJRN";
        check(::write(fd, torn, sizeof(torn)) == sizeof(torn)
                  && ::fsync(fd) == 0, "could not persist torn-tail fault");
        ::close(fd);
        rejects([&] { NativeTerminalReceiptJournal bad(path); },
                "torn terminal journal was silently truncated/replayed");
    }
    check(::unlink(path.c_str()) == 0 && ::rmdir(dir) == 0,
          "test journal cleanup failed");
    std::puts("NATIVE_DURABLE_TERMINAL_JOURNAL_PASS"
              " claim_once=1 crash_quarantine=1 external_proof=1"
              " lost_ack=1 torn_fail_closed=1 concurrency=1");
}
