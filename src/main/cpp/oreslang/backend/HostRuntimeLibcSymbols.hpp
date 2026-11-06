#pragma once
#include <algorithm>
#include <iterator>
#include <string_view>

namespace oreslang {

// Unprefixed libc/POSIX symbols that the native host runtimes linked by
// scripts/compile-aot.sh (NativeStringRegion.c, NativeRecordRegion.c,
// NativeIntArrayRegion.c) import, on macOS and Linux. A guest function emitted
// under one of these names would replace the libc definition for the runtime
// at link time (e.g. a guest `write` captured the console writer thread), so
// host codegen gives such guest functions a compiler-owned symbol instead.
// The source name, call resolution and Java-visible behaviour are unchanged.
//
// tests/native_runtime_libc_symbol_guard.cmake recompiles the runtimes and
// fails if they import an unprefixed symbol missing from this list.
inline constexpr std::string_view kHostRuntimeLibcSymbols[] = {
    "abort", "bcmp", "bzero", "calloc", "clock_gettime", "close", "dup",
    "fcntl", "free", "fstat", "isatty", "malloc", "memcmp", "memcpy",
    "memmove", "memset", "pipe", "poll", "pthread_create", "pthread_join",
    "pthread_sigmask", "read", "realloc", "sigaddset", "sigemptyset", "write",
};

inline bool is_host_runtime_libc_symbol(std::string_view name) noexcept {
    return std::find(std::begin(kHostRuntimeLibcSymbols),
                     std::end(kHostRuntimeLibcSymbols), name) !=
        std::end(kHostRuntimeLibcSymbols);
}

}  // namespace oreslang
