#pragma once

#include "oreslang/runtime/DbWalFrameV1.hpp"
#include "oreslang/runtime/DbWalReplayV1.hpp"
#include "oreslang/runtime/DbWalShardGrantRegistry.hpp"

// POSIX host-only physical WAL sync gate; NOT compiled Oreslang guest.
// A trusted runtime must supply an authenticated private directory FD, own
// actor/isolate permissions and pin the physical I/O worker lifetime.
// File locks are advisory and cannot exclude a hostile directory co-writer.
// Returned sync metadata is NOT a guest capability, logical commit or ACK.
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

#if defined(__linux__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#else
#error "DbDurableWalFile requires an audited POSIX target"
#endif

namespace oreslang::runtime {

// File-data sync for the WAL. macOS has no fdatasync(); F_FULLFSYNC is its durable
// equivalent (plain fsync may stop at the drive cache), falling back to fsync.
inline int db_wal_datasync(int fd) noexcept {
#if defined(__APPLE__)
    return ::fcntl(fd, F_FULLFSYNC) == 0 ? 0 : ::fsync(fd);
#else
    return ::fdatasync(fd);
#endif
}

class DbDurableWalFile final {
public:
    static constexpr std::size_t min_frame = 31;
    static constexpr std::size_t max_frame = 1118;

    // Only the trusted runtime may correlate this with authenticated
    // tenant/actor/handle identity. No constructor fields grant authority.
    struct SyncReceipt {
        std::uint32_t generation;
        std::uint32_t operation_id;
        std::uint64_t before;
        std::uint64_t after;
    };

    static std::unique_ptr<DbDurableWalFile> open_trusted(
            int directory_fd, const std::string& basename,
            std::uint32_t generation) {
        if (generation == 0 || basename.empty() || basename.size() > 100
                || basename == "." || basename == ".."
                || basename.find('/') != std::string::npos
                || basename.find('\\') != std::string::npos
                || basename.find('\0') != std::string::npos)
            throw std::invalid_argument("invalid DB WAL basename/generation");
        struct stat dir_info {};
        if (::fstat(directory_fd, &dir_info) != 0 || !S_ISDIR(dir_info.st_mode))
            throw std::invalid_argument("DB WAL root must be an open directory fd");
        const int fd = ::openat(directory_fd, basename.c_str(),
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw_os_error("openat WAL");
        try {
            struct stat info {};
            if (::fstat(fd, &info) != 0) throw_os_error("fstat WAL");
            if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
                throw std::runtime_error("WAL is not single-linked regular file");
            if (::flock(fd, LOCK_EX | LOCK_NB) != 0)
                throw_os_error("exclusive WAL lock");
            // fdatasync(fd) does NOT durably link a newly created WAL
            // filename into the parent directory. Require the already-
            // authenticated directory FD to complete fsync before this
            // owner can issue any physical append/sync receipt.
            // Unsupported filesystems or un-syncable directory FDs fail
            // closed; do not silently downgrade to file-only durability.
            if (::fsync(directory_fd) != 0)
                throw_os_error("WAL parent directory fsync");
            return std::unique_ptr<DbDurableWalFile>(
                    new DbDurableWalFile(fd, generation));
        } catch (...) {
            ::close(fd);
            throw;
        }
    }

    DbDurableWalFile(const DbDurableWalFile&) = delete;
    DbDurableWalFile& operator=(const DbDurableWalFile&) = delete;
    DbDurableWalFile(DbDurableWalFile&&) = delete;
    DbDurableWalFile& operator=(DbDurableWalFile&&) = delete;

    ~DbDurableWalFile() noexcept { close(); }

    /**
     * Strict WAL v1 append boundary. The tenant argument MUST be obtained
     * from an authenticated context-issued shard grant, never from guest
     * DATA. This validates the exact immutable buffer before file mutation.
     * No guest commit receipt, replay or directory sync is implied.
     */
    SyncReceipt append_validated_and_sync(std::uint32_t tenant,
                                          std::uint32_t generation,
                                          std::uint32_t operation_id,
                                          const std::uint8_t* frame,
                                          std::size_t frame_size) {
        if (!frame || frame_size < min_frame || frame_size > max_frame)
            throw std::invalid_argument("WAL frame outside bounded range");
        // Validate and physically write the SAME detached bytes. A guest or
        // caller buffer must not change the wire after integrity checking.
        // The caller still owes a stable, readable source for this copy.
        std::array<std::uint8_t, max_frame> detached {};
        std::memcpy(detached.data(), frame, frame_size);
        DbWalFrameV1::require_matching(
                detached.data(), frame_size, tenant, generation, operation_id);
        return append_and_sync(
                generation, operation_id, detached.data(), frame_size);
    }

    /**
     * Synchronous HOST-ONLY bridge: the authenticated caller passes a grant
     * previously issued for THIS file's fstat identity. Under the registry
     * lock, begin_write checks the complete grant and takes the physical
     * gate's exclusive slot atomically against drain. The actual fdatasync
     * executes while that slot is held. This worker retires it on BOTH
     * success and failure, including a partially written/uncertain record.
     *
     * No guest DATA may supply the grant or caller actor identity. No async
     * actor lifetime pin, guest ACK, or checkpoint publication is implied.
     */
    template<std::size_t Capacity>
    SyncReceipt append_authorized_and_sync(
            DbWalShardGrantRegistry<Capacity>& registry,
            const typename DbWalShardGrantRegistry<Capacity>::Grant& grant,
            std::uint64_t context_id, std::uint32_t tenant,
            std::uint32_t shard, std::uint64_t actor_id,
            std::uint32_t generation, std::uint32_t operation_id,
            const std::uint8_t* frame, std::size_t frame_size) {
        if (closed_ || uncertain_) throw std::logic_error(
                "WAL physical file closed or uncertain");
        struct stat info {};
        if (::fstat(fd_, &info) != 0) throw_os_error("WAL authorization fstat");
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
            throw std::logic_error("WAL physical file identity changed");
        typename DbWalShardGrantRegistry<Capacity>::FileIdentity identity{
            static_cast<std::uint64_t>(info.st_dev),
            static_cast<std::uint64_t>(info.st_ino)};
        if (!registry.begin_write(grant, context_id, tenant, shard,
                actor_id, generation, identity))
            throw std::logic_error("WAL shard grant admission refused");
        // A started synchronous physical worker must NEVER release the slot
        // based on a Future's logical cancellation. It owns retirement.
        try {
            auto receipt = append_validated_and_sync(
                    tenant, generation, operation_id, frame, frame_size);
            if (!registry.physical_write_retired(
                    grant, context_id, actor_id, generation))
                throw std::logic_error("WAL physical retirement unverified");
            return receipt;
        } catch (...) {
            // If a previous retirement unexpectedly failed, preserve
            // the gate rather than forging proof of physical completion.
            if (registry.physical_write_retired(
                    grant, context_id, actor_id, generation))
                throw;
            throw std::logic_error("WAL physical retirement lost; quarantine");
        }
    }

    // Low-level trusted physical primitive: callers entering from a DB
    // shard actor MUST use append_validated_and_sync with real grant identity.
    SyncReceipt append_and_sync(std::uint32_t generation,
                                std::uint32_t operation_id,
                                const std::uint8_t* frame,
                                std::size_t frame_size) {
        if (closed_ || uncertain_)
            throw std::logic_error("DB WAL closed or sync outcome unknown");
        if (generation != generation_ || operation_id == 0)
            throw std::invalid_argument("DB WAL generation/operation mismatch");
        if (!frame || frame_size < min_frame || frame_size > max_frame)
            throw std::invalid_argument("DB WAL invalid frame size");
        const off_t start = ::lseek(fd_, 0, SEEK_END);
        if (start < 0) { uncertain_ = true; throw_os_error("WAL size"); }
        if (static_cast<std::uint64_t>(start) >
                static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) - frame_size) {
            uncertain_ = true;
            throw std::overflow_error("DB WAL file length overflow");
        }
        std::size_t written = 0;
        while (written < frame_size) {
            const ssize_t n = ::write(fd_, frame + written, frame_size - written);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                uncertain_ = true;
                if (n < 0) throw_os_error("WAL write");
                throw std::runtime_error("WAL write made no progress");
            }
            written += static_cast<std::size_t>(n);
        }
        // fdatasync() completion is a necessary file-data sync condition,
        // not a parent directory sync or end-to-end database commit.
        if (db_wal_datasync(fd_) != 0) {
            uncertain_ = true;
            throw_os_error("WAL fdatasync");
        }
        return SyncReceipt{generation_, operation_id,
                static_cast<std::uint64_t>(start),
                static_cast<std::uint64_t>(start) + frame_size};
    }

    /**
     * Host-only, read-only replay preflight through this locked, trusted
     * directory-relative file handle. The segment generation can predate
     * the current new owner's generation. NEVER issue guest ACK or auto-truncate.
     * Caller must own the higher-level exclusive shard recovery fence.
     */
    DbWalReplayV1::Report scan_recovery(
            std::uint32_t tenant, std::uint32_t segment_generation) const {
        if (closed_ || uncertain_)
            throw std::logic_error("WAL closed/unknown; cannot scan");
        return DbWalReplayV1::scan(fd_, tenant, segment_generation,
                [](const std::uint8_t*, std::size_t, std::uint64_t) {});
    }

    std::uint64_t file_size() const {
        if (closed_) throw std::logic_error("WAL closed");
        struct stat info {};
        if (::fstat(fd_, &info) != 0) throw_os_error("WAL fstat");
        if (info.st_size < 0) throw std::runtime_error("negative WAL length");
        return static_cast<std::uint64_t>(info.st_size);
    }

    bool uncertain() const noexcept { return uncertain_; }
    void close() noexcept {
        if (!closed_) { closed_ = true; ::close(fd_); fd_ = -1; }
    }

private:
    explicit DbDurableWalFile(int fd, std::uint32_t generation)
        : fd_(fd), generation_(generation) {}

    [[noreturn]] static void throw_os_error(const char* call) {
        throw std::system_error(errno, std::generic_category(), call);
    }

    int fd_ = -1;
    std::uint32_t generation_ = 0;
    bool closed_ = false;
    bool uncertain_ = false;
};

} // namespace oreslang::runtime
