#pragma once

#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace oreslang::runtime {

// Native host I/O primitive ONLY. It is NOT registered as guest .ores API,
// an isolate constructor, or an asynchronous carrier-safe operation.
// The isolate runtime must supply a race-safely opened, exclusive WAL FD and
// issue the opaque actor incarnation key. Never accept raw guest paths/FDs.
class WalStorageFdOwner final {
public:
    struct Scope {
        std::uint32_t tenant, isolate, shard, actor, generation;
        bool operator==(const Scope& other) const noexcept {
            return tenant==other.tenant && isolate==other.isolate &&
                shard==other.shard && actor==other.actor && generation==other.generation;
        }
    };

    class SyncedReceipt final {
        friend class WalStorageFdOwner;
        Scope scope_;
        std::uint32_t sequence_, operation_;
        std::uint64_t end_offset_;
        std::vector<std::uint8_t> frame_;
        SyncedReceipt(Scope scope, std::uint32_t sequence,
                      std::uint32_t operation, std::uint64_t end,
                      std::vector<std::uint8_t> frame)
            : scope_(scope), sequence_(sequence), operation_(operation),
              end_offset_(end), frame_(std::move(frame)) {}
    public:
        Scope scope() const noexcept {return scope_;}
        std::uint32_t sequence() const noexcept {return sequence_;}
        std::uint32_t operation() const noexcept {return operation_;}
        std::uint64_t end_offset() const noexcept {return end_offset_;}
        // Full bounded frame instead of unauthenticated short hash; DO NOT
        // treat this as a transferable or cryptographic capability/receipt.
        const std::vector<std::uint8_t>& frame() const noexcept {return frame_;}
    };

    struct UnknownOutcome final : std::runtime_error {
        explicit UnknownOutcome(const std::string& reason)
            : std::runtime_error("WAL outcome unknown; quarantine: " + reason) {}
    };

    // Host-only adoption. Call on a disk worker, not an actor carrier.
    // FD custody is transferred to the owner only on success; caller must
    // close it on failure. Exclusive runtime lease/path validation and
    // parent-directory fsync on creation are strictly OUTSIDE this class.
    static WalStorageFdOwner* adopt_verified_fd(int fd, const void* activation,
                                                 Scope scope) {
        if (fd < 0 || activation == nullptr || scope.tenant == 0 ||
            scope.isolate == 0 || scope.shard == 0 || scope.actor == 0 ||
            scope.generation == 0) throw std::invalid_argument("invalid WAL runtime grant");
        // Linux pwrite() with O_APPEND can ignore the supplied offset.
        // Refuse such handles *before* accepting any WAL ownership; this
        // physical check is independent of path-confinement authorization.
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || (flags & O_APPEND) != 0)
            throw std::invalid_argument("WAL FD has unsafe append/unknown status flags");
        struct stat st {};
        if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
            st.st_nlink != 1 || st.st_size != 0)
            throw std::invalid_argument("WAL requires exclusive empty regular FD");
        return new WalStorageFdOwner(fd,activation,scope);
    }

    WalStorageFdOwner(const WalStorageFdOwner&) = delete;
    WalStorageFdOwner& operator=(const WalStorageFdOwner&) = delete;
    WalStorageFdOwner(WalStorageFdOwner&&) = delete;
    WalStorageFdOwner& operator=(WalStorageFdOwner&&) = delete;

    // For a shutdown that has *already* retired all physical I/O and actor
    // resource loans. The caller must guarantee no concurrent invocation
    // before releasing the last owning reference. No guest/actor carriers.
    ~WalStorageFdOwner() noexcept {
        revoked_.store(true,std::memory_order_release);
        if (fd_ >= 0) (void)::close(fd_);
    }

    void revoke() noexcept { revoked_.store(true,std::memory_order_release); }
    bool quarantined() const noexcept {return poisoned_.load(std::memory_order_acquire);}

    // BLOCKING POSIX primitive. The owning isolate must invoke this only
    // from a bounded native DISK EXECUTOR after retaining an actor retirement
    // loan. A caller on a scheduler carrier is a programming error, not async.
    // No guest-visible Committed API is supplied by this class.
    SyncedReceipt write_and_sync_on_disk_worker(const void* activation, Scope claimed,
                                                const std::vector<std::uint8_t>& input) {
        if (activation != activation_ || !(claimed == scope_))
            throw std::invalid_argument("foreign or stale native activation");
        auto fields = verify_frame(input);
        if (fields.tenant != scope_.tenant || fields.generation != scope_.generation)
            throw std::invalid_argument("foreign WAL frame identity");
        std::lock_guard<std::mutex> guard(lock_);
        if (revoked_.load(std::memory_order_acquire) || poisoned_.load(std::memory_order_acquire))
            throw std::logic_error("native WAL owner revoked/quarantined");
        if (fields.sequence != next_sequence_ || seen_.count(fields.operation) != 0)
            throw std::invalid_argument("sequence gap or duplicate operation");
        if (seen_.size() >= 4096 || input.size() > 1048576 - accepted_bytes_)
            throw std::length_error("bounded WAL or idempotency history exhausted");
        seen_.insert(fields.operation);
        accepted_bytes_ += input.size();
        ++next_sequence_;
        // From this point, any failure or revocation has unknown physical
        // effects. Do NOT rollback sequence, retry, skip, or close the FD.
        try {
            // O_APPEND can be enabled after adoption through a duplicated
            // descriptor sharing the same open-file description. Detect it
            // immediately before positional writes and quarantine as UNKNOWN.
            // This does not replace exclusive physical FD custody.
            const int live_flags = ::fcntl(fd_, F_GETFL);
            if (live_flags < 0 || (live_flags & O_APPEND) != 0)
                throw UnknownOutcome("WAL descriptor status flags changed");
            struct stat st {};
            if (::fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode)
                || st.st_nlink != 1 || st.st_size < 0
                || static_cast<std::uint64_t>(st.st_size) != synced_end_)
                throw UnknownOutcome("external FD mutation or custody lost");
            std::size_t total = 0;
            while (total < input.size()) {
                ssize_t n = ::pwrite(fd_, input.data() + total,
                                     input.size() - total,
                                     static_cast<off_t>(synced_end_ + total));
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) throw UnknownOutcome("write failure/zero progress");
                total += static_cast<std::size_t>(n);
            }
            for (;;) {
                if (::fdatasync(fd_) == 0) break;
                if (errno != EINTR) throw UnknownOutcome("fdatasync failed");
            }
            synced_end_ += total;
            if (revoked_.load(std::memory_order_acquire))
                throw UnknownOutcome("revoked after dispatch/sync");
            return SyncedReceipt(scope_,fields.sequence,fields.operation,synced_end_,input);
        } catch (...) {
            poisoned_.store(true,std::memory_order_release);
            throw;
        }
    }

private:
    struct Fields {std::uint32_t tenant,generation,sequence,operation;};
    static std::uint32_t byte(const std::vector<std::uint8_t>& b, std::size_t i) {return b[i];}
    static std::uint32_t u16(const std::vector<std::uint8_t>& b, std::size_t i) {
        return byte(b,i) | byte(b,i+1)<<8;
    }
    static std::uint32_t i31(const std::vector<std::uint8_t>& b, std::size_t i) {
        if (byte(b,i+3)>127) throw std::invalid_argument("high-bit WAL identity");
        return byte(b,i) | byte(b,i+1)<<8 | byte(b,i+2)<<16 | byte(b,i+3)<<24;
    }
    static Fields verify_frame(const std::vector<std::uint8_t>& b) {
        if (b.size()<31 || b.size()>1118 || b[0]!=79 || b[1]!=68 || b[2]!=66
            || b[3]!=49 || b[4]!=1 || (b[5]!=1 && b[5]!=2))
            throw std::invalid_argument("invalid WAL magic/kind/length");
        Fields fields{i31(b,6),i31(b,10),i31(b,14),i31(b,18)};
        auto key = u16(b,22),value=u16(b,24);
        if (!fields.tenant || !fields.generation || !fields.sequence || !fields.operation
            || !key || key>64 || value>1024 || (b[5]==2 && value!=0)
            || b.size()!=30+key+value)
            throw std::invalid_argument("invalid WAL identity or payload bounds");
        std::uint32_t a=1,c=0;
        for (std::size_t i=0;i<b.size()-4;++i) {
            a=(a+byte(b,i))%65521;c=(c+a)%65521;
        }
        if (u16(b,b.size()-4)!=a || u16(b,b.size()-2)!=c)
            throw std::invalid_argument("WAL checksum mismatch");
        return fields;
    }
    WalStorageFdOwner(int fd, const void* activation, Scope scope)
        : fd_(fd),activation_(activation),scope_(scope) {}
    int fd_;
    const void* activation_;
    Scope scope_;
    std::mutex lock_;
    std::atomic<bool> revoked_{false};
    std::atomic<bool> poisoned_{false};
    std::unordered_set<std::uint32_t> seen_;
    std::uint32_t next_sequence_=1;
    std::size_t accepted_bytes_=0;
    std::uint64_t synced_end_=0;
};

} // namespace oreslang::runtime
