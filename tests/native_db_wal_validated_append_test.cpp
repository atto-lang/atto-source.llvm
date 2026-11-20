// Host POSIX strict ODB1 WAL append+fdatasync; no compiled Oreslang guest.
#include "oreslang/runtime/DbDurableWalFile.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using oreslang::runtime::DbDurableWalFile;

static void u16(std::array<std::uint8_t, 31>& f, int at, unsigned x) {
    f[at] = static_cast<std::uint8_t>(x);
    f[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
static void u32(std::array<std::uint8_t, 31>& f, int at, unsigned x) {
    u16(f, at, x); u16(f, at + 2, x >> 16);
}
static std::array<std::uint8_t, 31> frame(unsigned tenant,
        unsigned gen, unsigned seq, unsigned op) {
    std::array<std::uint8_t, 31> f{};
    f[0] = 79; f[1] = 68; f[2] = 66; f[3] = 49;
    f[4] = 1; f[5] = 1;
    u32(f, 6, tenant); u32(f, 10, gen);
    u32(f, 14, seq); u32(f, 18, op);
    u16(f, 22, 1); u16(f, 24, 0);
    f[26] = 0x55;
    unsigned a = 1, b = 0;
    for (unsigned i = 0; i < 27; ++i) {
        a = (a + f[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    u16(f, 27, a); u16(f, 29, b);
    return f;
}
template <typename F> static void denied(F f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
}

int main() {
    char name[] = "/tmp/ores-wal-validated-XXXXXX";
    char* dir = ::mkdtemp(name);
    assert(dir);
    int root = ::open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(root >= 0);
    {
        auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 4);
        auto bytes = frame(7, 4, 1, 99);
        denied([&] {
            (void)wal->append_validated_and_sync(
                    8, 4, 99, bytes.data(), bytes.size());
        });
        denied([&] {
            (void)wal->append_validated_and_sync(
                    7, 5, 99, bytes.data(), bytes.size());
        });
        denied([&] {
            (void)wal->append_validated_and_sync(
                    7, 4, 100, bytes.data(), bytes.size());
        });
        auto broken = bytes;
        broken[26] ^= 1;
        denied([&] {
            (void)wal->append_validated_and_sync(
                    7, 4, 99, broken.data(), broken.size());
        });
        assert(wal->file_size() == 0);
        auto receipt = wal->append_validated_and_sync(
                7, 4, 99, bytes.data(), bytes.size());
        assert(receipt.before == 0 && receipt.after == 31);
        assert(receipt.generation == 4 && receipt.operation_id == 99);
        assert(!wal->uncertain());
    }
    // A separate process appends+fdatasync, exits; parent reads/reopens.
    // This is not a physical power-cut or metadata durability test.
    pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        try {
            auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 5);
            auto bytes = frame(7, 5, 2, 100);
            auto r = wal->append_validated_and_sync(
                    7, 5, 100, bytes.data(), bytes.size());
            ::_exit(r.before == 31 && r.after == 62 ? 0 : 20);
        } catch (...) { ::_exit(21); }
    }
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    {
        auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 6);
        assert(wal->file_size() == 62);
    }
    assert(::unlinkat(root, "wal.odb", 0) == 0);
    assert(::close(root) == 0);
    assert(::rmdir(dir) == 0);
    return 0;
}
