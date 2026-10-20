#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>

#if defined(__unix__) || defined(__APPLE__)
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

// Link these symbols from the exact compiler-emitted verified LLVM module.
// The ABI and layout remain native C-compatible; Oreslang guest object
// references are i32 fields, not native pointer ABI parameters.
extern "C" std::int64_t oreslang_test_read_compact(
    void* source, void* arena, std::uint64_t capacity, void* original);
extern "C" void oreslang_test_write_compact(
    void* source, void* arena, std::uint64_t capacity,
    void* original, std::int64_t value);

struct alignas(8) Node {
    std::int64_t value;
    std::uint32_t ref;
    std::uint32_t padding;
};
static_assert(sizeof(Node) == 16);
static_assert(offsetof(Node, ref) == 8);

struct Proof {
    const void* base;
    std::uint64_t bytes;
    std::uint32_t original_encoding;
    std::uint64_t original_generation;
    std::uint64_t active_generation;
    bool live;
};

// Deliberately TRUSTED test-host implementation. A real runtime would use
// unforgeable supervisor-admitted heap/turn domain identity, allocation
// generation, and exact stored reference from an out-of-line proof ledger.
// It MUST NOT recover a fresh generation from the offset.
extern "C" bool oreslang_test_original_proof_check(
    void* raw, void* base, std::uint64_t span, std::uint32_t ref) {
    if (!raw) return false;
    const auto& p = *static_cast<const Proof*>(raw);
    return p.live && p.base == base && p.bytes == span &&
           p.original_encoding == ref && p.original_generation != 0 &&
           p.original_generation == p.active_generation;
}

static void require(bool cond, const char* why) {
    if (!cond) throw std::runtime_error(why);
}

#if defined(__unix__) || defined(__APPLE__)
template<typename F>
static void must_trap(F f, const char* why) {
    const auto pid = fork();
    require(pid >= 0, "fork death-test failed");
    if (pid == 0) {
        f();
        _exit(0);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid, "waitpid death-test failed");
    // SIGSEGV is explicitly NOT accepted: it could signify that the compiler
    // performed the memory access without a checked trap.
    require(WIFSIGNALED(status) &&
                (WTERMSIG(status) == SIGILL || WTERMSIG(status) == SIGTRAP ||
                 WTERMSIG(status) == SIGABRT), why);
}
#endif

int main() {
    try {
        // Use a C++ buffer with a proven eight-byte-aligned base.
        alignas(16) std::array<std::byte, 64> arena{};
        Node source{11, 3, 0};   // offset 16 -> encoded ((16>>3) + 1) == 3
        Node target{77, 0, 0};
        std::memcpy(arena.data(), &source, sizeof source);
        std::memcpy(arena.data() + 16, &target, sizeof target);
        Proof proof{arena.data(), arena.size(), 3, 100, 100, true};

        auto* first = arena.data();
        require(oreslang_test_read_compact(first, arena.data(), arena.size(),
                                            &proof) == 77,
                "LLVM checked ref32 load returned wrong target value");
        oreslang_test_write_compact(first, arena.data(), arena.size(), &proof, 99);
        std::memcpy(&target, arena.data() + 16, sizeof target);
        require(target.value == 99, "LLVM checked ref32 store did not commit");

#if defined(__unix__) || defined(__APPLE__)
        proof.active_generation = 101;
        must_trap([&] {
            (void)oreslang_test_read_compact(first, arena.data(), arena.size(), &proof);
        }, "stale generation failed to trap before dereference");
        proof.active_generation = 100;

        proof.live = false;
        must_trap([&] {
            (void)oreslang_test_read_compact(first, arena.data(), arena.size(), &proof);
        }, "retired target failed to trap before dereference");
        proof.live = true;

        must_trap([&] {
            (void)oreslang_test_read_compact(first, arena.data() + 8, 56, &proof);
        }, "foreign arena base failed to trap");

        std::uint32_t null_ref = 0;
        std::memcpy(arena.data() + 8, &null_ref, sizeof null_ref);
        must_trap([&] {
            (void)oreslang_test_read_compact(first, arena.data(), arena.size(), &proof);
        }, "null compressed field failed to trap");
        std::uint32_t stale_ref = 3;
        std::memcpy(arena.data() + 8, &stale_ref, sizeof stale_ref);

        proof.bytes = 16;
        must_trap([&] {
            (void)oreslang_test_read_compact(first, arena.data(), 16, &proof);
        }, "span below target allocation failed to trap");
#endif
        std::cout << "PASS LLVM compact i32 physical load/store, stale/null/foreign/"
                     "out-of-bounds fail-closed access\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL ref32 runtime e2e: " << e.what() << "\n";
        return 1;
    }
}
