#pragma once

// POSIX host-only physical WAL sync gate; NOT compiled Oreslang guest.
// A trusted runtime must supply an authenticated private directory FD, own
// actor/isolate permissions and pin the physical I/O worker lifetime.
// File locks are advisory and cannot exclude a hostile directory co-writer.
// Returned sync metadata is NOT a guest capability, logical commit or ACK.
#include <cerrno>
#include <cstdint>
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
        if (::fdatasync(fd_) != 0) {
            uncertain_ = true;
            throw_os_error("WAL fdatasync");
        }
        return SyncReceipt{generation_, operation_id,
                static_cast<std::uint64_t>(start),
                static_cast<std::uint64_t>(start) + frame_size};
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
