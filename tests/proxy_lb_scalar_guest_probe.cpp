#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

// Linux host caller reads the actual i64 Oreslang guest entry point from
// clang-linked LLVM IR, not the OS i32/8-bit process exit wrapper.
// This is native guest scalar execution, NOT proxy/actor/network parity.
int main(int argc, char** argv) {
    if (argc != 3) return 64;
    char* end = nullptr;
    const long long expected = std::strtoll(argv[2], &end, 10);
    if (end == argv[2] || *end != '\0') return 65;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        std::fprintf(stderr, "cannot load generated LLVM guest library\n");
        return 66;
    }
    using Entry = std::int64_t (*)();
    // ISO C++ itself does not define converting dlsym to function pointer;
    // POSIX dlsym does, and this gate is Linux-only.
    void* symbol = dlsym(library, "__oreslang_guest_main");
    if (!symbol) {
        std::fprintf(stderr, "missing LLVM native i64 guest symbol\n");
        dlclose(library);
        return 67;
    }
    Entry entry = nullptr;
    static_assert(sizeof(entry) == sizeof(symbol));
    std::memcpy(&entry, &symbol, sizeof(entry));
    const auto observed = entry();
    dlclose(library);
    if (observed != expected) {
        std::fprintf(stderr, "native scalar mismatch: %lld != %lld\n",
                     static_cast<long long>(observed), expected);
        return 68;
    }
    std::printf("LLVM_PROXY_LB_SCALAR_NATIVE_PASS=%lld\n",
                static_cast<long long>(observed));
    return 0;
}
