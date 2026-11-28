#include "oreslang/runtime/TenantVmGuestLoader.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <climits>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#if defined(__linux__)
#include <sys/mman.h>
#endif

namespace oreslang::runtime::vm {

namespace {

constexpr std::size_t kMaxLibraryBytes = std::size_t{256} << 20;

std::uint32_t rotate(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

constexpr std::array<std::uint32_t, 64> kRound = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be,
    0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa,
    0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85,
    0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256_block(std::array<std::uint32_t, 8>& h, const std::uint8_t* p) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (std::uint32_t{p[i * 4]} << 24) | (std::uint32_t{p[i * 4 + 1]} << 16) | (std::uint32_t{p[i * 4 + 2]} << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t t1 = k + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const std::uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
}

bool read_all(int fd, std::vector<std::uint8_t>& out) {
    struct stat info {};
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size <= 0 ||
        static_cast<std::uint64_t>(info.st_size) > kMaxLibraryBytes)
        return false;
    out.resize(static_cast<std::size_t>(info.st_size));
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = read(fd, out.data() + done, out.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

bool write_all(int fd, const std::vector<std::uint8_t>& bytes) {
    std::size_t done = 0;
    while (done < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + done, bytes.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::string make_private_directory(std::string& error) {
    const char* base = std::getenv("TMPDIR");
    std::string pattern = std::string(base && *base ? base : "/tmp") + "/ores-guest-XXXXXX";
    std::vector<char> directory(pattern.begin(), pattern.end());
    directory.push_back('\0');
    if (!mkdtemp(directory.data())) {
        error = "cannot prepare a private directory";
        return "";
    }
    // The dynamic linker may record the resolved path, and the check after loading compares names.
    char resolved[PATH_MAX];
    if (!realpath(directory.data(), resolved)) {
        rmdir(directory.data());
        error = "cannot resolve the private directory";
        return "";
    }
    return resolved;
}

// Map exactly the bytes that were hashed, so nothing can swap the file after the check.
//
// The name has to be unique for the life of the process. The dynamic linker returns a library it
// already has when a name matches, so reusing one, as /proc/self/fd/N does when N comes around
// again, would hand this tenant whatever code was loaded under it before. dladdr confirms it.
void* open_verified(const std::vector<std::uint8_t>& bytes, std::string& name, std::string& error) {
    const std::string directory = make_private_directory(error); // private to this user, gone before anything runs
    if (directory.empty()) return nullptr;
    name = directory + "/guest.lib";
    void* handle = nullptr;
#if defined(__linux__)
    const int fd = memfd_create("ores-tenant-guest", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || !write_all(fd, bytes) ||
        fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
        error = "cannot prepare sealed copy";
    } else if (symlink(("/proc/self/fd/" + std::to_string(fd)).c_str(), name.c_str()) != 0) {
        error = "cannot name sealed copy";
    } else {
        handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) error = dlerror();
    }
#else
    const int fd = open(name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0500);
    if (fd < 0 || !write_all(fd, bytes)) {
        error = "cannot write private copy";
    } else {
        handle = dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle) error = dlerror();
    }
#endif
    if (fd >= 0) close(fd);
    unlink(name.c_str());
    rmdir(directory.c_str());
    return handle;
}

} // namespace

std::string sha256_hex(const void* data, std::size_t size) {
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto* p = static_cast<const std::uint8_t*>(data);
    std::size_t left = size;
    for (; left >= 64; left -= 64, p += 64) sha256_block(h, p);
    std::uint8_t last[128] = {};
    if (left) std::memcpy(last, p, left);
    last[left] = 0x80;
    const std::size_t total = left < 56 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) last[total - 1 - i] = static_cast<std::uint8_t>(bits >> (i * 8));
    sha256_block(h, last);
    if (total == 128) sha256_block(h, last + 64);
    char text[65];
    for (int i = 0; i < 8; ++i) std::snprintf(text + i * 8, 9, "%08x", h[i]);
    return std::string(text, 64);
}

LoadedGuest load_guest_library_checked(const std::string& path, const std::string& expected,
                                        GuestPreloadVerifier verifier, void* context) {
    LoadedGuest out;
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        out.error = "cannot open guest library";
        return out;
    }
    std::vector<std::uint8_t> bytes;
    const bool complete = read_all(fd, bytes);
    close(fd);
    if (!complete) {
        out.error = "guest library is not a readable regular file of acceptable size";
        return out;
    }
    if (sha256_hex(bytes.data(), bytes.size()) != expected) {
        out.error = "guest library does not match its digest";
        return out;
    }
    // This is the exact byte buffer later copied into the sealed execution
    // image. Never reopen a path for signature validation: it can be swapped.
    // The callback is invoked BEFORE any code is mapped or its module exports
    // resolved, and its result is not a guest bearer capability.
    if (verifier && !verifier(bytes.data(), bytes.size(), context)) {
        out.error = "guest library signed admission rejected";
        return out;
    }
    std::string name;
    void* handle = open_verified(bytes, name, out.error);
    if (!handle) return out;
    std::shared_ptr<void> owner(handle, [](void* h) { dlclose(h); });
    const auto* module = static_cast<const OresVmGuestModule*>(dlsym(handle, ORES_VM_GUEST_MODULE_SYMBOL));
    if (!module) {
        out.error = "guest library has no module";
        return out;
    }
    // The module must be in the library just mapped. dlsym also searches what that library
    // depends on, and a dynamic linker that reused an older library would land here too.
    Dl_info where {};
    if (dladdr(module, &where) == 0 || !where.dli_fname || name != where.dli_fname) {
        out.error = "guest library resolved to something other than the verified bytes";
        return out;
    }
    if (module->abi_version != ORES_VM_GUEST_ABI_VERSION) {
        out.error = "guest library was built for another ABI version";
        return out;
    }
    out.module = module;
    out.owner = owner;
    return out;
}

LoadedGuest load_guest_library(const std::string& path, const std::string& expected) {
    // Backward-compatible ABI-v4 loader. It does NOT grant privileged I/O,
    // which must instead pass the mandatory signed admission and v5 gates.
    return load_guest_library_checked(path, expected, nullptr, nullptr);
}

} // namespace oreslang::runtime::vm
