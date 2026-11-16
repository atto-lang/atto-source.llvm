// Linux/POSIX host-only WAL append+fdatasync exercise, not guest execution.
#include "oreslang/runtime/DbDurableWalFile.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using oreslang::runtime::DbDurableWalFile;

int main() {
    char path[] = "/tmp/ores-db-wal-native-XXXXXX";
    char* dir = ::mkdtemp(path);
    assert(dir != nullptr);
    const int root = ::open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    assert(root >= 0);
#if defined(__linux__)
    // O_PATH is a valid directory-relative lookup authority, but not a
    // syncable directory FD. Open must fail rather than issue a WAL owner
    // whose just-created filename has no parent-directory fsync receipt.
    const int path_only = ::open(dir, O_PATH | O_DIRECTORY | O_CLOEXEC);
    assert(path_only >= 0);
    bool unsynced_denied = false;
    try {
        (void)DbDurableWalFile::open_trusted(
                path_only, "unsynced.odb", 7);
    } catch (const std::system_error& error) {
        unsynced_denied = error.code().value() == EBADF;
    }
    assert(unsynced_denied);
    assert(::close(path_only) == 0);
    assert(::unlinkat(root, "unsynced.odb", 0) == 0);
#endif
    std::array<std::uint8_t, 31> frame {};
    frame.fill(0x5a);
    {
        auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 7);
        assert(wal->file_size() == 0);
        bool denied = false;
        try { (void)wal->append_and_sync(8, 1, frame.data(), frame.size()); }
        catch (const std::invalid_argument&) { denied = true; }
        assert(denied && wal->file_size() == 0);
        denied = false;
        try { (void)wal->append_and_sync(7, 0, frame.data(), frame.size()); }
        catch (const std::invalid_argument&) { denied = true; }
        assert(denied);
        denied = false;
        try { (void)wal->append_and_sync(7, 1, frame.data(), 30); }
        catch (const std::invalid_argument&) { denied = true; }
        assert(denied);
        denied = false;
        try { (void)DbDurableWalFile::open_trusted(root, "wal.odb", 8); }
        catch (const std::system_error&) { denied = true; }
        assert(denied);
        auto receipt = wal->append_and_sync(7, 42, frame.data(), frame.size());
        assert(receipt.before == 0 && receipt.after == 31 && receipt.generation == 7);
        assert(!wal->uncertain());
    }
    // Separate process completes fdatasync and exits with _exit, then
    // another owner reopens. Not a power-cut simulation.
    const pid_t child = ::fork();
    assert(child >= 0);
    if (child == 0) {
        try {
            auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 9);
            auto receipt = wal->append_and_sync(9, 43, frame.data(), frame.size());
            if (receipt.before != 31 || receipt.after != 62) ::_exit(20);
            ::_exit(0);
        } catch (...) { ::_exit(21); }
    }
    int status = 0;
    assert(::waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    {
        auto wal = DbDurableWalFile::open_trusted(root, "wal.odb", 10);
        assert(wal->file_size() == 62);
    }
    assert(::symlinkat("wal.odb", root, "escape") == 0);
    bool denied = false;
    try { (void)DbDurableWalFile::open_trusted(root, "escape", 11); }
    catch (const std::system_error&) { denied = true; }
    assert(denied);
    denied = false;
    try { (void)DbDurableWalFile::open_trusted(root, "../escape", 11); }
    catch (const std::invalid_argument&) { denied = true; }
    assert(denied);
    assert(::unlinkat(root, "escape", 0) == 0);
    assert(::unlinkat(root, "wal.odb", 0) == 0);
    assert(::close(root) == 0);
    assert(::rmdir(dir) == 0);
    std::puts("LLVM_HOST_DURABLE_WAL_SMOKE_PASS");
}
