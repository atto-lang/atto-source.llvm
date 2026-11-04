#pragma once
#include "scoped_permission_policy.hpp"
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#if defined(__linux__)
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace oreslang::permissions {

// Trusted HOST-only Linux adapter, not a guest API or an OS-wide sandbox.
// Unsupported targets/kernel syscalls MUST deny; there is no open() fallback.
class ScopedFileBoundary final {
public:
    static constexpr std::size_t max_transfer = 1024 * 1024;
    static std::string read_existing(const ScopedPolicy& policy,
                                     std::string_view absolute_path,
                                     std::size_t limit = max_transfer) {
        const std::string path(absolute_path); // identical policy/syscall identity
        if (limit > max_transfer || !policy.allows(Permission::Read, path))
            throw std::domain_error("read permission denied");
#if defined(__linux__) && defined(SYS_openat2)
        Descriptor fd = open_regular(path, O_RDONLY);
        std::string output;
        std::array<char, 4096> bytes{};
        while (true) {
            const std::size_t available = limit - output.size() + 1;
            const std::size_t count = available < bytes.size() ? available : bytes.size();
            const ssize_t got = ::read(fd.fd, bytes.data(), count);
            if (got < 0 && errno == EINTR) continue;
            if (got < 0) throw std::system_error(errno, std::generic_category(), "scoped read");
            if (got == 0) return output;
            output.append(bytes.data(), static_cast<std::size_t>(got));
            if (output.size() > limit) throw std::length_error("scoped read limit exceeded");
        }
#else
        (void)path;
        throw std::domain_error("scoped filesystem boundary unsupported");
#endif
    }

    static void append_existing(const ScopedPolicy& policy,
                                std::string_view absolute_path,
                                std::string_view bytes) {
        const std::string path(absolute_path);
        if (bytes.size() > max_transfer || !policy.allows(Permission::Write, path))
            throw std::domain_error("write permission denied");
#if defined(__linux__) && defined(SYS_openat2)
        Descriptor fd = open_regular(path, O_WRONLY | O_APPEND);
        std::size_t written = 0;
        while (written < bytes.size()) {
            const ssize_t count = ::write(fd.fd, bytes.data() + written, bytes.size() - written);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) throw std::system_error(errno, std::generic_category(), "scoped append");
            if (count == 0) throw std::system_error(EIO, std::generic_category(), "zero-byte append");
            written += static_cast<std::size_t>(count);
        }
#else
        (void)path;
        (void)bytes;
        throw std::domain_error("scoped filesystem boundary unsupported");
#endif
    }

private:
#if defined(__linux__) && defined(SYS_openat2)
    struct Descriptor {
        int fd = -1;
        explicit Descriptor(int owned) noexcept : fd(owned) {}
        ~Descriptor() noexcept { if (fd >= 0) ::close(fd); }
        Descriptor(const Descriptor&) = delete;
        Descriptor& operator=(const Descriptor&) = delete;
    };
    static int release(Descriptor& d) noexcept {
        const int result = d.fd;
        d.fd = -1;
        return result;
    }
    static Descriptor open_regular(const std::string& path, int mode) {
        // ScopedPolicy rejects nonabsolute, dot, dotdot, double-slash and
        // control-containing paths before the open. Anchor beneath root.
        Descriptor root(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
        if (root.fd < 0) throw std::system_error(errno, std::generic_category(), "open root");
        struct open_how how{};
        how.flags = static_cast<std::uint64_t>(mode | O_CLOEXEC | O_NOFOLLOW);
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_XDEV;
        const int opened = static_cast<int>(::syscall(SYS_openat2, root.fd,
            path.c_str() + 1, &how, sizeof(how)));
        if (opened < 0) throw std::system_error(errno, std::generic_category(), "scoped openat2");
        Descriptor file(opened);
        struct stat metadata{};
        if (::fstat(file.fd, &metadata) != 0)
            throw std::system_error(errno, std::generic_category(), "scoped fstat");
        if (!S_ISREG(metadata.st_mode))
            throw std::domain_error("scoped path is not a regular file");
        return Descriptor(release(file));
    }
#endif
};
} // namespace oreslang::permissions
