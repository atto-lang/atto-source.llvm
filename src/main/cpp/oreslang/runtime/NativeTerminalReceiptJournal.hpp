#pragma once
/*
 * Trusted-host crash journal for native supervisor terminal receipts.
 *
 * One locked writer per absolute path. Every state transition is a canonical
 * 176-byte, checksummed, fsync'd append-only frame. Replay rejects torn
 * suffixes, forged transitions and duplicate claims, without truncation.
 * This is an opt-in storage primitive: a native callback/scheduler still
 * must authorize the exact physical actor and terminal event separately.
 *
 * FNV64 detects accidental torn/corrupt frames; it is NOT a cryptographic
 * signature. The file and its containing directory must be trusted by the
 * host, not exposed to guests. fsync cannot prove a remote effect occurred.
 */
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace oreslang::runtime {

struct DurableTerminalKey {
    // Supervisor/runtime and exact physical actor generation binding.
    std::array<std::uint64_t, 13> words{};
    bool operator<(const DurableTerminalKey& other) const noexcept {
        return words < other.words;
    }
    bool operator==(const DurableTerminalKey& other) const noexcept {
        return words == other.words;
    }
    static DurableTerminalKey from(
        std::uint64_t supervisor, std::uint64_t runtime,
        std::uint64_t parent_domain, std::uint64_t parent_slot,
        std::uint64_t parent_generation, std::uint64_t child_domain,
        std::uint64_t child_slot, std::uint64_t child_generation,
        std::uint64_t kind, std::uint64_t code, std::uint64_t correlation,
        std::uint64_t logical_child, std::uint64_t incarnation) {
        return {{{supervisor, runtime, parent_domain, parent_slot,
                  parent_generation, child_domain, child_slot,
                  child_generation, kind, code, correlation,
                  logical_child, incarnation}}};
    }
    bool valid() const noexcept {
        const auto& w = words;
        return w[0] && w[1] && w[2] && w[4] && w[5] && w[7]
            && w[2] == w[5] && w[1] == w[2] && w[8] >= 1 && w[8] <= 3
            && w[11] && w[12];
    }
};

enum class DurableTerminalPhase : std::uint8_t {
    recorded = 1, physical_done = 2, claimed = 3,
    effect_proven = 4, acknowledged = 5
};

struct DurableTerminalState {
    DurableTerminalPhase phase = DurableTerminalPhase::recorded;
    std::uint64_t nonce = 0;
    std::array<std::uint8_t, 32> external_evidence_sha256{};
    bool ambiguous() const noexcept {
        return phase == DurableTerminalPhase::claimed;
    }
};

/* ExternalEffectProof is not an assertion that an effect happened: its
 * binding must be checked against an authoritative external commit ledger.
 * The stored digest is the SHA-256 of that ledger's canonical evidence
 * record, computed by the trusted host, not by this journal. */
struct ExternalEffectProof {
    DurableTerminalKey key;
    std::uint64_t nonce = 0;
    std::array<std::uint8_t, 32> evidence_sha256{};
};

class NativeTerminalReceiptJournal final {
public:
    using Phase = DurableTerminalPhase;
    using Key = DurableTerminalKey;
    using State = DurableTerminalState;
    static constexpr std::size_t kFrameSize = 176;

    explicit NativeTerminalReceiptJournal(const std::string& absolute_path) {
        if (absolute_path.size() < 2 || absolute_path.front() != '/')
            throw std::invalid_argument("journal path must be absolute");
        struct stat st{};
        const bool existed = (::lstat(absolute_path.c_str(), &st) == 0);
        if (!existed && errno != ENOENT)
            throw std::runtime_error("journal lstat failed");
        int flags = O_RDWR | O_APPEND | O_CLOEXEC | O_NOFOLLOW;
        if (!existed) flags |= O_CREAT | O_EXCL;
        fd_ = ::open(absolute_path.c_str(), flags, 0600);
        if (fd_ < 0) throw std::runtime_error("journal open failed");
        try {
            if (::flock(fd_, LOCK_EX | LOCK_NB) != 0)
                throw std::runtime_error("journal has another writer");
            if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)
                || (st.st_mode & 0077) != 0)
                throw std::runtime_error("journal must be private regular file");
            if (!existed) {
                const auto slash = absolute_path.find_last_of('/');
                const std::string dir = slash == 0
                    ? "/" : absolute_path.substr(0, slash);
                int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (d < 0) throw std::runtime_error("journal parent open failed");
                const int result = ::fsync(d);
                (void)::close(d);
                if (result != 0)
                    throw std::runtime_error("journal parent fsync failed");
            }
            load();
        } catch (...) {
            (void)::close(fd_);
            fd_ = -1;
            throw;
        }
    }
    ~NativeTerminalReceiptJournal() {
        if (fd_ >= 0) (void)::close(fd_);
    }
    NativeTerminalReceiptJournal(const NativeTerminalReceiptJournal&) = delete;
    NativeTerminalReceiptJournal& operator=(const NativeTerminalReceiptJournal&) = delete;

    State inspect(const Key& key) const {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = states_.find(key);
        if (it == states_.end()) throw std::logic_error("unknown terminal identity");
        return it->second;
    }

    // Durable record before any external CONTROL effect. A repeated event
    // with the same key is NOT another independent invocation.
    void record(const Key& key) {
        if (!key.valid()) throw std::invalid_argument("invalid terminal identity");
        append(key, Phase::recorded, 0, {});
    }
    // The trusted host must test scheduler's exact poll_done (not a cached
    // READY flag) on the same generation. False cannot mint DONE evidence.
    bool record_physical_done(const Key& key,
                              const std::function<bool()>& live_done_probe) {
        if (!live_done_probe || !live_done_probe()) return false;
        append(key, Phase::physical_done, 0, {});
        return true;
    }
    // A CLAIM is durable *before* the host executes an external effect.
    // After a crash, CLAIMED is ambiguous and must NEVER be auto-claimed.
    std::uint64_t claim(const Key& key) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = states_.find(key);
        if (it == states_.end() || it->second.phase != Phase::physical_done)
            throw std::logic_error("terminal claim requires physical DONE");
        if (highest_nonce_ == UINT64_MAX)
            throw std::overflow_error("journal claim nonce exhausted");
        const auto nonce = highest_nonce_ + 1;
        append_locked(key, Phase::claimed, nonce, {});
        return nonce;
    }
    // Caller-supplied bytes alone have no authority. The trusted verifier
    // must consult the real external durable effect ledger, check exact key,
    // nonce, and effect identity, and reject missing/ambiguous evidence.
    bool prove_external_effect(
            const ExternalEffectProof& proof,
            const std::function<bool(const ExternalEffectProof&)>& verifier) {
        if (!verifier || !proof.nonce || all_zero(proof.evidence_sha256))
            return false;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            const auto it = states_.find(proof.key);
            if (it == states_.end() || it->second.phase != Phase::claimed
                || it->second.nonce != proof.nonce) return false;
        }
        // Do not hold the journal mutex while waiting on external storage.
        if (!verifier(proof)) return false;
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = states_.find(proof.key);
        if (it == states_.end() || it->second.phase != Phase::claimed
            || it->second.nonce != proof.nonce) return false;
        append_locked(proof.key, Phase::effect_proven, proof.nonce,
                      proof.evidence_sha256);
        return true;
    }
    void acknowledge(const ExternalEffectProof& proof) {
        std::lock_guard<std::mutex> guard(mutex_);
        const auto it = states_.find(proof.key);
        if (it == states_.end() || it->second.phase != Phase::effect_proven
            || it->second.nonce != proof.nonce
            || it->second.external_evidence_sha256 != proof.evidence_sha256)
            throw std::logic_error("ACK requires same verified external effect");
        append_locked(proof.key, Phase::acknowledged, proof.nonce,
                      proof.evidence_sha256);
    }

private:
    int fd_ = -1;
    mutable std::mutex mutex_;
    std::map<Key, State> states_;
    std::uint64_t sequence_ = 0;
    std::uint64_t highest_nonce_ = 0;
    bool poisoned_ = false;
    using Frame = std::array<std::uint8_t, kFrameSize>;

    static bool all_zero(const std::array<std::uint8_t, 32>& digest) {
        return std::all_of(digest.begin(), digest.end(),
                           [](std::uint8_t byte) { return byte == 0; });
    }
    static void put64(Frame& frame, std::size_t offset, std::uint64_t x) {
        for (unsigned i = 0; i != 8; ++i)
            frame[offset + i] = static_cast<std::uint8_t>(x >> (8 * i));
    }
    static std::uint64_t get64(const Frame& frame, std::size_t offset) {
        std::uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i)
            value |= std::uint64_t(frame[offset + i]) << (8 * i);
        return value;
    }
    static std::uint64_t checksum(const Frame& frame) {
        std::uint64_t hash = UINT64_C(14695981039346656037);
        for (std::size_t i = 0; i < 168; ++i) {
            hash ^= frame[i];
            hash *= UINT64_C(1099511628211);
        }
        return hash;
    }
    static Frame encode(std::uint64_t seq, const Key& key, Phase event,
                        std::uint64_t nonce,
                        const std::array<std::uint8_t, 32>& digest) {
        Frame frame{};
        constexpr char magic[] = "OREJRNL1";
        std::memcpy(frame.data(), magic, 8);
        frame[8] = 1;
        frame[9] = static_cast<std::uint8_t>(event);
        put64(frame, 16, seq);
        for (std::size_t i = 0; i < 13; ++i)
            put64(frame, 24 + 8 * i, key.words[i]);
        put64(frame, 128, nonce);
        std::copy(digest.begin(), digest.end(), frame.begin() + 136);
        put64(frame, 168, checksum(frame));
        return frame;
    }
    static void apply(std::map<Key, State>& states,
                      std::uint64_t& highest_nonce, const Key& key,
                      Phase event, std::uint64_t nonce,
                      const std::array<std::uint8_t, 32>& digest) {
        if (!key.valid()) throw std::runtime_error("corrupt journal identity");
        auto it = states.find(key);
        if (event == Phase::recorded) {
            if (it != states.end() || nonce || !all_zero(digest))
                throw std::runtime_error("duplicate or corrupt terminal record");
            states.emplace(key, State{});
            return;
        }
        if (it == states.end())
            throw std::runtime_error("orphan journal transition");
        auto& state = it->second;
        switch (event) {
        case Phase::physical_done:
            if (state.phase != Phase::recorded || nonce || !all_zero(digest))
                throw std::runtime_error("physical DONE out of order");
            break;
        case Phase::claimed:
            if (state.phase != Phase::physical_done || !nonce
                || nonce <= highest_nonce || !all_zero(digest))
                throw std::runtime_error("duplicate, stale or forged claim");
            highest_nonce = nonce;
            break;
        case Phase::effect_proven:
            if (state.phase != Phase::claimed || nonce != state.nonce
                || all_zero(digest))
                throw std::runtime_error("unverified external effect transition");
            break;
        case Phase::acknowledged:
            if (state.phase != Phase::effect_proven || nonce != state.nonce
                || digest != state.external_evidence_sha256)
                throw std::runtime_error("false terminal ACK");
            break;
        default: throw std::runtime_error("unknown journal event");
        }
        state.phase = event;
        if (event == Phase::claimed) state.nonce = nonce;
        if (event == Phase::effect_proven)
            state.external_evidence_sha256 = digest;
    }
    void append(const Key& key, Phase event, std::uint64_t nonce,
                const std::array<std::uint8_t, 32>& digest) {
        std::lock_guard<std::mutex> guard(mutex_);
        append_locked(key, event, nonce, digest);
    }
    void append_locked(const Key& key, Phase event, std::uint64_t nonce,
                       const std::array<std::uint8_t, 32>& digest) {
        if (poisoned_ || fd_ < 0)
            throw std::runtime_error("journal is quarantined after I/O failure");
        if (sequence_ == UINT64_MAX)
            throw std::overflow_error("journal sequence exhausted");
        // Validate and allocate ALL new state before writing durable bytes.
        auto next = states_;
        std::uint64_t next_highest = highest_nonce_;
        apply(next, next_highest, key, event, nonce, digest);
        const Frame frame = encode(sequence_ + 1, key, event, nonce, digest);
        std::size_t offset = 0;
        while (offset < frame.size()) {
            const auto n = ::write(fd_, frame.data() + offset,
                                   frame.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                poisoned_ = true;
                throw std::runtime_error("journal write failed: outcome ambiguous");
            }
            offset += static_cast<std::size_t>(n);
        }
        if (::fsync(fd_) != 0) {
            poisoned_ = true;
            throw std::runtime_error("journal fsync failed: outcome ambiguous");
        }
        states_.swap(next);
        highest_nonce_ = next_highest;
        ++sequence_;
    }
    void load() {
        if (::lseek(fd_, 0, SEEK_SET) < 0)
            throw std::runtime_error("journal seek failed");
        Frame frame{};
        for (;;) {
            std::size_t nread = 0;
            while (nread < frame.size()) {
                const auto n = ::read(fd_, frame.data() + nread,
                                      frame.size() - nread);
                if (n < 0 && errno == EINTR) continue;
                if (n < 0) throw std::runtime_error("journal read failed");
                if (n == 0) break;
                nread += static_cast<std::size_t>(n);
            }
            if (nread == 0) break;
            if (nread != frame.size()
                || std::memcmp(frame.data(), "OREJRNL1", 8) != 0
                || frame[8] != 1 || get64(frame, 168) != checksum(frame))
                throw std::runtime_error("torn or corrupt journal; quarantine");
            for (std::size_t i = 10; i < 16; ++i)
                if (frame[i] != 0)
                    throw std::runtime_error("invalid journal reserved bits");
            if (sequence_ == UINT64_MAX
                || get64(frame, 16) != sequence_ + 1)
                throw std::runtime_error("journal sequence mismatch");
            Key key{};
            for (std::size_t i = 0; i < 13; ++i)
                key.words[i] = get64(frame, 24 + i * 8);
            std::array<std::uint8_t, 32> digest{};
            std::copy(frame.begin() + 136, frame.begin() + 168,
                      digest.begin());
            apply(states_, highest_nonce_, key,
                  static_cast<Phase>(frame[9]),
                  get64(frame, 128), digest);
            ++sequence_;
        }
    }
};

} // namespace oreslang::runtime
