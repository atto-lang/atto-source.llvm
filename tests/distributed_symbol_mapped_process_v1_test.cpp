// POSIX host-only proof of immutable UUID image sharing and snapshot replacement.
// This is not a production catalog loader: the test owns all files and never
// mutates/truncates a published inode. Untrusted mutable files are not safe mmap inputs.
#include "DistributedSymbolManifestV1.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using oreslang::distributed_symbol_v1::Uuid;
using oreslang::distributed_symbol_v1::valid;
namespace {
const Uuid A{0x00,0x11,0x22,0x33,0x44,0x55,0x46,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff};
const Uuid B{0xff,0xee,0xdd,0xcc,0xbb,0xaa,0x49,0x88,0xb7,0x66,0x55,0x44,0x33,0x22,0x11,0x00};
void check(bool ok) { if (!ok) throw std::runtime_error("mapped UUID proof failed"); }
class Mapping {
public:
    explicit Mapping(const std::string& path) {
        const int fd = open(path.c_str(), O_RDONLY);
        check(fd >= 0);
        struct stat st{};
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 16) {
            close(fd); throw std::runtime_error("invalid UUID image extent");
        }
        data_ = mmap(nullptr, 16, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        check(data_ != MAP_FAILED);
    }
    ~Mapping() { munmap(data_, 16); }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Uuid value() const {
        Uuid id{};
        std::copy_n(static_cast<const std::uint8_t*>(data_), id.size(), id.begin());
        check(valid(id)); return id;
    }
private:
    void* data_ = MAP_FAILED;
};
struct TempDir {
    char path[32] = "/tmp/atto-symbol-v1-XXXXXX";
    TempDir() { check(mkdtemp(path) != nullptr); }
    ~TempDir() {
        unlink((std::string(path) + "/image").c_str());
        unlink((std::string(path) + "/next").c_str());
        rmdir(path);
    }
};
void publish(const std::string& dir, const Uuid& id) {
    const auto next = dir + "/next";
    const auto image = dir + "/image";
    const int fd = open(next.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    check(fd >= 0);
    const auto n = write(fd, id.data(), id.size());
    const int mode = fchmod(fd, 0400);
    const int closed = close(fd);
    check(n == static_cast<ssize_t>(id.size()) && mode == 0 && closed == 0);
    check(rename(next.c_str(), image.c_str()) == 0);
}
void child(const char* executable, const std::string& path, const char* expected) {
    const auto pid = fork();
    check(pid >= 0);
    if (pid == 0) {
        execl(executable, executable, "--read", path.c_str(), expected, static_cast<char*>(nullptr));
        _exit(127);
    }
    int status = 0;
    pid_t got;
    do { got = waitpid(pid, &status, 0); } while (got < 0 && errno == EINTR);
    check(got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 4 && std::string(argv[1]) == "--read") {
        const std::string expected = argv[3];
        check(expected == "A" || expected == "B");
        check(Mapping(argv[2]).value() == (expected == "A" ? A : B));
        return 0;
    }
    check(argc == 1);
    TempDir dir;
    const auto path = std::string(dir.path) + "/image";
    publish(dir.path, A);
    const Mapping original(path);
    check(original.value() == A);
    child(argv[0], path, "A");
    publish(dir.path, B);
    const Mapping replacement(path);
    check(original.value() == A && replacement.value() == B);
    child(argv[0], path, "B");
    check(unlink(path.c_str()) == 0);
    check(original.value() == A && replacement.value() == B);
    std::cout << "POSIX UUID image: exec readers and retained snapshots passed\n";
}
