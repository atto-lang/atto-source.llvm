#pragma once
#include <array>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <cstdint>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#endif

#if defined(__unix__) || defined(__APPLE__)
// Descriptor metadata is checked both before and after consuming bytes.
// This detects ordinary in-place replacement, truncation and extension
// during compilation. It is a best-effort stability check, not a guarantee
// of an atomic snapshot against hostile concurrent writers.
inline bool ores_source_stat_stable(const struct stat& before, const struct stat& after) {
#if defined(__APPLE__)
  const bool same_times =
      before.st_mtimespec.tv_sec == after.st_mtimespec.tv_sec &&
      before.st_mtimespec.tv_nsec == after.st_mtimespec.tv_nsec &&
      before.st_ctimespec.tv_sec == after.st_ctimespec.tv_sec &&
      before.st_ctimespec.tv_nsec == after.st_ctimespec.tv_nsec;
#else
  const bool same_times =
      before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
      before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
      before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
      before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
#endif
  return S_ISREG(after.st_mode) &&
      before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      before.st_size == after.st_size && same_times;
}
#endif

struct SourceIdentity {
  std::uint64_t device = 0, inode = 0, size = 0;
  std::int64_t mtime_sec = 0, mtime_nsec = 0;
  std::int64_t ctime_sec = 0, ctime_nsec = 0;
  bool valid = false;
};

#if defined(__unix__) || defined(__APPLE__)
inline SourceIdentity ores_source_identity(const struct stat& info) {
  SourceIdentity identity;
  identity.device = static_cast<std::uint64_t>(info.st_dev);
  identity.inode = static_cast<std::uint64_t>(info.st_ino);
  identity.size = static_cast<std::uint64_t>(info.st_size);
#if defined(__APPLE__)
  identity.mtime_sec = static_cast<std::int64_t>(info.st_mtimespec.tv_sec);
  identity.mtime_nsec = static_cast<std::int64_t>(info.st_mtimespec.tv_nsec);
  identity.ctime_sec = static_cast<std::int64_t>(info.st_ctimespec.tv_sec);
  identity.ctime_nsec = static_cast<std::int64_t>(info.st_ctimespec.tv_nsec);
#else
  identity.mtime_sec = static_cast<std::int64_t>(info.st_mtim.tv_sec);
  identity.mtime_nsec = static_cast<std::int64_t>(info.st_mtim.tv_nsec);
  identity.ctime_sec = static_cast<std::int64_t>(info.st_ctim.tv_sec);
  identity.ctime_nsec = static_cast<std::int64_t>(info.st_ctim.tv_nsec);
#endif
  identity.valid = true;
  return identity;
}

inline bool ores_source_identity_equal(
    const SourceIdentity& expected, const SourceIdentity& observed) {
  return expected.valid && observed.valid &&
      expected.device == observed.device &&
      expected.inode == observed.inode &&
      expected.size == observed.size &&
      expected.mtime_sec == observed.mtime_sec &&
      expected.mtime_nsec == observed.mtime_nsec &&
      expected.ctime_sec == observed.ctime_sec &&
      expected.ctime_nsec == observed.ctime_nsec;
}

inline SourceIdentity capture_source_identity(
    const char* path, std::size_t limit, const char* overflow) {
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error(std::string("cannot open source: ") + path);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot inspect source file");
  if (!S_ISREG(info.st_mode)) throw std::runtime_error("source must be a regular file");
  if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > limit)
    throw std::runtime_error(overflow);
  return ores_source_identity(info);
}
#else
inline bool ores_source_identity_equal(
    const SourceIdentity& expected, const SourceIdentity& observed) {
  return expected.valid && observed.valid &&
      expected.device == observed.device && expected.inode == observed.inode &&
      expected.size == observed.size &&
      expected.mtime_sec == observed.mtime_sec &&
      expected.mtime_nsec == observed.mtime_nsec;
}
inline SourceIdentity capture_source_identity(
    const char* path, std::size_t limit, const char* overflow) {
  std::error_code error;
  const auto status = std::filesystem::status(path, error);
  if (error) throw std::runtime_error(std::string("cannot inspect source: ") + path);
  if (!std::filesystem::is_regular_file(status))
    throw std::runtime_error("source must be a regular file");
  const auto size = std::filesystem::file_size(path, error);
  if (error) throw std::runtime_error("cannot inspect source file size");
  if (size > limit) throw std::runtime_error(overflow);
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error("cannot inspect source modification time");
  SourceIdentity identity;
  identity.size = static_cast<std::uint64_t>(size);
  const auto ticks = modified.time_since_epoch().count();
  identity.mtime_sec = static_cast<std::int64_t>(ticks);
  identity.valid = true;
  return identity;
}
#endif

// Check the opened descriptor, so a path replacement cannot turn a regular
// source into a blocking FIFO between admission and reading. Regular symlinks
// remain supported. The byte budget also applies if a file grows during reads.
inline std::string read_regular_source(
    const char* path, std::size_t limit, const char* overflow,
    SourceIdentity* identity = nullptr,
    const SourceIdentity* expected_identity = nullptr) {
  std::array<char, 8192> buffer{};
  std::string text;
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error(std::string("cannot open source: ") + path);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot inspect source file");
  if (!S_ISREG(info.st_mode)) throw std::runtime_error("source must be a regular file");
  if (info.st_size < 0 || static_cast<std::uintmax_t>(info.st_size) > limit)
    throw std::runtime_error(overflow);
  const SourceIdentity opened_identity = ores_source_identity(info);
  if (expected_identity != nullptr &&
      (!expected_identity->valid ||
       !ores_source_identity_equal(*expected_identity, opened_identity))) {
    throw std::runtime_error("source identity changed between resolution and read");
  }
  for (;;) {
    const auto count = ::read(fd, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error("failed reading source file");
    }
    if (count == 0) break;
    if (static_cast<std::size_t>(count) > limit - text.size())
      throw std::runtime_error(overflow);
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  struct stat after{};
  if (::fstat(fd, &after) != 0 || !ores_source_stat_stable(info, after) ||
      static_cast<std::uintmax_t>(after.st_size) != text.size())
    throw std::runtime_error("source changed during read");
  if (identity) *identity = opened_identity;
#else
  // Portable best-effort identity binding. POSIX additionally binds the
  // opened descriptor's device/inode; portable hosts bind size + mtime before
  // and after the stream read so replacement/mutation is still fail-closed
  // for ordinary filesystem changes.
  const SourceIdentity opened_identity =
      capture_source_identity(path, limit, overflow);
  if (expected_identity != nullptr &&
      (!expected_identity->valid ||
       !ores_source_identity_equal(*expected_identity, opened_identity))) {
    throw std::runtime_error("source identity changed between resolution and read");
  }
  std::ifstream source(path, std::ios::binary);
  if (!source) throw std::runtime_error(std::string("cannot open source: ") + path);
  while (source) {
    source.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = source.gcount();
    if (count <= 0) break;
    if (static_cast<std::size_t>(count) > limit - text.size())
      throw std::runtime_error(overflow);
    text.append(buffer.data(), static_cast<std::size_t>(count));
  }
  if (source.bad() || (source.fail() && !source.eof()))
    throw std::runtime_error("failed reading source file");
  const SourceIdentity after_identity =
      capture_source_identity(path, limit, overflow);
  if (!ores_source_identity_equal(opened_identity, after_identity) ||
      text.size() != opened_identity.size) {
    throw std::runtime_error("source changed during read");
  }
  if (identity) *identity = opened_identity;
#endif
  return text;
}

// Reject malformed Unicode (overlong encodings, surrogates, truncated input and
// code points above U+10FFFF) and binary NUL before native parser admission.
inline void require_valid_source_utf8(std::string_view source) {
    for (std::size_t i = 0; i < source.size();) {
        const auto lead = static_cast<unsigned char>(source[i]);
        if (lead == 0) throw std::runtime_error("native source contains NUL byte");
        if (lead == 0x1b || lead == 0x7f)
            throw std::runtime_error("native source contains terminal control byte");
        if (lead <= 0x7f) { ++i; continue; }

        std::size_t continuation_count = 0;
        unsigned char min_first = 0x80;
        unsigned char max_first = 0xbf;
        if (lead >= 0xc2 && lead <= 0xdf) {
            continuation_count = 1;
        } else if (lead >= 0xe0 && lead <= 0xef) {
            continuation_count = 2;
            if (lead == 0xe0) min_first = 0xa0;
            if (lead == 0xed) max_first = 0x9f;
        } else if (lead >= 0xf0 && lead <= 0xf4) {
            continuation_count = 3;
            if (lead == 0xf0) min_first = 0x90;
            if (lead == 0xf4) max_first = 0x8f;
        } else {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        if (source.size() - i <= continuation_count) {
            throw std::runtime_error("native source is not valid UTF-8");
        }
        for (std::size_t j = 1; j <= continuation_count; ++j) {
            const auto byte = static_cast<unsigned char>(source[i + j]);
            if (byte < (j == 1 ? min_first : 0x80) ||
                byte > (j == 1 ? max_first : 0xbf)) {
                throw std::runtime_error("native source is not valid UTF-8");
            }
        }
        // Trojan Source: reject bidi marks/overrides/isolates even in comments.
        // The UTF-8 sequence is already validated above.
        if ((lead == 0xd8 && static_cast<unsigned char>(source[i + 1]) == 0x9c) ||
            (lead == 0xe2 &&
              ((static_cast<unsigned char>(source[i + 1]) == 0x80 &&
                 (static_cast<unsigned char>(source[i + 2]) == 0x8e ||
                  static_cast<unsigned char>(source[i + 2]) == 0x8f ||
                  (static_cast<unsigned char>(source[i + 2]) >= 0xaa &&
                   static_cast<unsigned char>(source[i + 2]) <= 0xae))) ||
               (static_cast<unsigned char>(source[i + 1]) == 0x81 &&
                static_cast<unsigned char>(source[i + 2]) >= 0xa6 &&
                static_cast<unsigned char>(source[i + 2]) <= 0xa9))))
            throw std::runtime_error("source contains bidirectional Unicode control");
        i += continuation_count + 1;
    }
}
