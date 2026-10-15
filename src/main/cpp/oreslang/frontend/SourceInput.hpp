#pragma once
#include <array>
#include <cerrno>
#include <fstream>
#include <stdexcept>
#include <string>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <filesystem>
#endif

// Check the opened descriptor, so a path replacement cannot turn a regular
// source into a blocking FIFO between admission and reading. Regular symlinks
// remain supported. The byte budget also applies if a file grows during reads.
inline std::string read_regular_source(const char* path, std::size_t limit,
                                       const char* overflow) {
  std::array<char, 8192> buffer{};
  std::string text;
#if defined(__unix__) || defined(__APPLE__)
  const int fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error(std::string("cannot open source: ") + path);
  struct Close { int fd; ~Close() { ::close(fd); } } close{fd};
  struct stat info{};
  if (::fstat(fd, &info) != 0) throw std::runtime_error("cannot inspect source file");
  if (!S_ISREG(info.st_mode)) throw std::runtime_error("source must be a regular file");
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
#else
  // Portable admission; the descriptor-level nonblocking guarantee is POSIX.
  if (!std::filesystem::is_regular_file(path))
    throw std::runtime_error("source must be a regular file");
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
#endif
  return text;
}
