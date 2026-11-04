#pragma once

// Canonical AttoLang filesystem admission for POSIX compiler hosts.
// Every ancestor is opened relative to the previous pinned directory handle,
// with O_NOFOLLOW. This is stronger than an lstat()/canonical() preflight:
// path substitution by a symlink cannot redirect the file descriptor.
// The caller still must fstat() the returned fd, enforce its byte budget,
// check that the descriptor is a regular file, and bind read identity.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

inline bool ores_pinned_atto_input(const char* path) {
    const auto filename = std::filesystem::path(path).filename().string();
    if (filename == ".atto.pkg.toml") return true;
    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return lower.size() >= 5 && lower.compare(lower.size() - 5, 5, ".atto") == 0;
}

inline int ores_open_source_fd(const char* path) {
    constexpr int read_flags = O_RDONLY | O_NONBLOCK | O_CLOEXEC;
    if (!ores_pinned_atto_input(path)) return ::open(path, read_flags);

    const std::filesystem::path source(path);
    std::vector<std::string> parts;
    for (const auto& component : source) {
        const auto name = component.string();
        if (name.empty() || name == "/" || name == ".") continue;
        parts.push_back(name);
    }
    if (parts.empty()) throw std::runtime_error("invalid canonical AttoLang input path");

    constexpr int dir_flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
    int directory_fd = ::open(source.is_absolute() ? "/" : ".", dir_flags);
    if (directory_fd < 0) throw std::runtime_error("cannot open canonical AttoLang source root");
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const bool leaf = index + 1 == parts.size();
        const int flags = leaf ? read_flags | O_NOFOLLOW : dir_flags;
        const int next = ::openat(directory_fd, parts[index].c_str(), flags);
        const int open_error = errno;
        ::close(directory_fd);
        if (next < 0) {
            if (open_error == ELOOP || open_error == ENOTDIR)
                throw std::runtime_error("symlinked AttoLang source path is not allowed");
            throw std::runtime_error("cannot open canonical AttoLang input without following links");
        }
        directory_fd = next;
    }
    return directory_fd;
}
#endif
