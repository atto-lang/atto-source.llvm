#pragma once
// POSIX same-host durable one-shot claim. Host-only, not guest API or multi-host CAS.
#include "AgentHostDispatchGate.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <openssl/evp.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace oreslang::agent_dispatch {
inline std::string local_claim_filename(const Scope& s) {
  if (!safe_id(s.tenant_id) || !safe_id(s.actor_id) ||
      !safe_id(s.dispatch_id) || !lower_hex(s.exact_revision, 40) ||
      !lower_hex(s.context_sha256, 64) || s.generation == 0 || s.epoch == 0)
    return {};
  // Context digest must NOT be in filename: changed payload for the same
  // logical dispatch collides, including after actor restart or code reload.
  const std::string key = "ores-agent-dispatch-claim-v1\n" + s.tenant_id + "\n" +
      s.actor_id + "\n" + s.dispatch_id + "\n";
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned int size = 0;
  if (EVP_Digest(key.data(), key.size(), hash.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32)
    return {};
  constexpr char hex[] = "0123456789abcdef";
  std::string name;
  name.reserve(70);
  for (unsigned i = 0; i < size; ++i) {
    name.push_back(hex[hash[i] >> 4]);
    name.push_back(hex[hash[i] & 15]);
  }
  return name + ".claim";
}
// Walk from a root directory descriptor, never following a symlink in ANY
// component. O_NOFOLLOW on the final path alone is insufficient.
inline int open_local_claim_directory(const std::string& absolute) {
  if (absolute.empty() || absolute.front() != '/' || absolute.size() == 1)
    return -1;
  int current = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (current < 0) return -1;
  std::size_t start = 1;
  while (start < absolute.size()) {
    const std::size_t end = absolute.find('/', start);
    const std::string component = absolute.substr(start, end - start);
    if (component.empty() || component == "." || component == ".." ||
        component.find('\0') != std::string::npos) {
      ::close(current);
      return -1;
    }
    const int next = ::openat(current, component.c_str(),
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    ::close(current);
    if (next < 0) return -1;
    current = next;
    if (end == std::string::npos) break;
    start = end + 1;
    if (start == absolute.size()) {
      ::close(current);
      return -1;
    }
  }
  return current;
}
class DurableLocalDispatchClaim final {
 public:
  explicit DurableLocalDispatchClaim(std::string private_directory)
      : directory_(std::move(private_directory)) {}
  bool claim_once(const Scope& s) const {
    const auto name = local_claim_filename(s);
    if (name.empty()) return false;
    const int dir = open_local_claim_directory(directory_);
    if (dir < 0) return false;
    struct stat st {};
    const bool private_dir = ::fstat(dir, &st) == 0 && S_ISDIR(st.st_mode) &&
        st.st_uid == ::geteuid() && (st.st_mode & 0777) == 0700;
    if (!private_dir) { ::close(dir); return false; }
    const int fd = ::openat(dir, name.c_str(), O_WRONLY | O_CREAT | O_EXCL |
                            O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) { ::close(dir); return false; }
    const std::string record = "ORES_AGENT_CLAIM_V1\n" + s.context_sha256 + "\n";
    std::size_t offset = 0;
    bool ok = true;
    while (offset < record.size()) {
      const ssize_t n = ::write(fd, record.data() + offset, record.size() - offset);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) { ok = false; break; }
      offset += static_cast<std::size_t>(n);
    }
    if (ok && ::fsync(fd) != 0) ok = false;
    if (::close(fd) != 0) ok = false;
    if (ok && ::fsync(dir) != 0) ok = false;
    if (::close(dir) != 0) ok = false;
    // Failure leaves the reserved name in place: never unlink on ambiguity.
    return ok;
  }
 private:
  std::string directory_;
};
} // namespace oreslang::agent_dispatch
