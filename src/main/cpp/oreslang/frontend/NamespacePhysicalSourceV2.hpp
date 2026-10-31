#pragma once

#include <cerrno>
#include <cstddef>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

// A physical-file reader, NOT a package authority: its caller must supply a
// separately approved source inventory and immutable manifest/source pins.
// Never trust a path spelling, canonical() result, or source digest on its own.
namespace oreslang::frontend {

struct NamespacePhysicalBytesV2 {
    std::string manifest_bytes;
    std::map<std::string, std::string> source_bytes;
};

inline bool namespace_v2_safe_member(std::string_view path) {
    if (path.size() < 6 || path.size() > 4096 || path.front() == '/' ||
            path.find('\\') != path.npos || path.find(':') != path.npos ||
            path.substr(path.size() - 5) != ".ores") return false;
    std::size_t at = 0;
    for (;;) {
        const auto end = path.find('/', at);
        auto part = path.substr(at, end == path.npos ? path.npos : end - at);
        if (part.empty() || part == "." || part == "..") return false;
        for (unsigned char ch : part) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.'))
                return false;
        }
        if (end == path.npos) return true;
        at = end + 1;
    }
}

#if defined(__unix__) || defined(__APPLE__)
namespace namespace_v2_physical_detail {
struct Fd {
    int fd = -1;
    explicit Fd(int value = -1) : fd(value) {}
    ~Fd() { if (fd >= 0) ::close(fd); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : fd(other.fd) { other.fd = -1; }
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) { if (fd >= 0) ::close(fd); fd = other.fd; other.fd = -1; }
        return *this;
    }
};
inline Fd directory_at(int parent, const std::string& component) {
    Fd result(::openat(parent, component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (result.fd < 0) throw std::runtime_error("v2 physical directory missing or symlinked");
    return result;
}
inline Fd physical_root(const std::filesystem::path& root) {
    if (!root.is_absolute() || root != root.lexically_normal())
        throw std::invalid_argument("v2 physical root must be absolute and canonical spelling");
    Fd directory(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (directory.fd < 0) throw std::runtime_error("cannot open physical root filesystem");
    for (const auto& part : root.relative_path()) {
        const std::string name = part.string();
        if (name.empty() || name == "." || name == "..")
            throw std::invalid_argument("v2 physical root has unsafe component");
        directory = directory_at(directory.fd, name);
    }
    return directory;
}
inline bool unchanged(const struct stat& lhs, const struct stat& rhs) {
#if defined(__APPLE__)
    const bool time_same = lhs.st_mtimespec.tv_sec == rhs.st_mtimespec.tv_sec &&
        lhs.st_mtimespec.tv_nsec == rhs.st_mtimespec.tv_nsec &&
        lhs.st_ctimespec.tv_sec == rhs.st_ctimespec.tv_sec &&
        lhs.st_ctimespec.tv_nsec == rhs.st_ctimespec.tv_nsec;
#else
    const bool time_same = lhs.st_mtim.tv_sec == rhs.st_mtim.tv_sec &&
        lhs.st_mtim.tv_nsec == rhs.st_mtim.tv_nsec &&
        lhs.st_ctim.tv_sec == rhs.st_ctim.tv_sec &&
        lhs.st_ctim.tv_nsec == rhs.st_ctim.tv_nsec;
#endif
    return S_ISREG(lhs.st_mode) && S_ISREG(rhs.st_mode) &&
        lhs.st_dev == rhs.st_dev && lhs.st_ino == rhs.st_ino &&
        lhs.st_size == rhs.st_size && time_same;
}
// An open descriptor remains valid after its pathname is replaced. Before
// publishing bytes, also bind the original directory entry to that inode.
// This is a checked boundary, not an atomic filesystem freeze: verified
// callers must compile the captured bytes and must never re-open the path.
inline void require_same_path_binding(int directory, const std::string& filename,
                                      const struct stat& originally_opened) {
    struct stat named{};
    if (::fstatat(directory, filename.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 ||
        !unchanged(originally_opened, named))
        throw std::runtime_error("v2 source pathname changed during capture");
}
inline std::string read_file(int directory, const std::string& filename, std::size_t limit) {
    Fd handle(::openat(directory, filename.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC));
    if (handle.fd < 0) throw std::runtime_error("v2 regular source/manifest unavailable or symlinked");
    struct stat before{};
    if (::fstat(handle.fd, &before) != 0 || !S_ISREG(before.st_mode) ||
        before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > limit)
        throw std::runtime_error("v2 nonregular or oversized source/manifest");
    std::string data;
    char buffer[8192];
    for (;;) {
        const ssize_t size = ::read(handle.fd, buffer, sizeof(buffer));
        if (size < 0) { if (errno == EINTR) continue; throw std::runtime_error("v2 read failed"); }
        if (size == 0) break;
        if (static_cast<std::size_t>(size) > limit - data.size())
            throw std::runtime_error("v2 read exceeded byte budget");
        data.append(buffer, static_cast<std::size_t>(size));
    }
    struct stat after{};
    if (::fstat(handle.fd, &after) != 0 || !unchanged(before, after) ||
        data.size() != static_cast<std::size_t>(before.st_size))
        throw std::runtime_error("v2 file changed during read");
    require_same_path_binding(directory, filename, before);
    return data;
}
inline void forbid_nested_manifest(int dir) {
    struct stat marker{};
    if (::fstatat(dir, ".ores.pkg.toml", &marker, AT_SYMLINK_NOFOLLOW) == 0)
        throw std::runtime_error("v2 nested package authority forbidden");
    if (errno != ENOENT) throw std::runtime_error("v2 nested package marker cannot be inspected");
}
inline std::string member_bytes(int root, std::string_view rel) {
    Fd directory(::dup(root));
    if (directory.fd < 0) throw std::runtime_error("v2 physical source root unavailable");
    std::size_t cursor = 0;
    for (;;) {
        const auto slash = rel.find('/', cursor);
        const auto name = std::string(rel.substr(cursor, slash == rel.npos ? rel.npos : slash - cursor));
        if (slash == rel.npos) return read_file(directory.fd, name, 8u * 1024u * 1024u);
        directory = directory_at(directory.fd, name);
        forbid_nested_manifest(directory.fd);
        cursor = slash + 1;
    }
}
} // namespace namespace_v2_physical_detail
#endif

inline NamespacePhysicalBytesV2 read_namespace_v2_physical_files(
        const std::filesystem::path& trusted_physical_root,
        const std::vector<std::string>& trusted_manifest_members) {
#if defined(__unix__) || defined(__APPLE__)
    using namespace namespace_v2_physical_detail;
    if (trusted_manifest_members.empty() || trusted_manifest_members.size() > 4096)
        throw std::invalid_argument("v2 physical source inventory count is invalid");
    Fd root = physical_root(trusted_physical_root);
    NamespacePhysicalBytesV2 result;
    result.manifest_bytes = read_file(root.fd, ".ores.pkg.toml", 1024u * 1024u);
    std::size_t total = 0;
    for (const auto& relative : trusted_manifest_members) {
        if (!namespace_v2_safe_member(relative))
            throw std::invalid_argument("v2 physical member has noncanonical path");
        std::string contents = member_bytes(root.fd, relative);
        if (contents.size() > 64u * 1024u * 1024u - total)
            throw std::invalid_argument("v2 physical sources exceed aggregate byte budget");
        total += contents.size();
        if (!result.source_bytes.emplace(relative, std::move(contents)).second)
            throw std::invalid_argument("v2 physical inventory repeats member");
    }
    return result;
#else
    (void)trusted_physical_root;
    (void)trusted_manifest_members;
    throw std::runtime_error("v2 physical namespace source admission requires POSIX no-follow descriptors");
#endif
}

} // namespace oreslang::frontend
