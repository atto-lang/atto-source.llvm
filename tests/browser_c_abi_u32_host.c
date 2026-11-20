/* P0 proof of compiler-created C ABI width, not a tab/session backend.
 * Call EXACT uint32_t(void), never reinterpret guest i64 as C uint32_t.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static const char *const forbidden[] = {
    "ores_browser_open", "ores_browser_dispatch",
    "ores_browser_snapshot", "ores_browser_close"
};

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') return 2;
    void *handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        fputs("C_U32_ABI_FAIL: unable to load guest ELF\n", stderr);
        return 3;
    }
    void *host_symbol = dlsym(handle, "ores_browser_abi_version");
    if (!host_symbol || dlsym(handle, "browser_native_abi_v1") != NULL)
        return 4;
    uint32_t (*version)(void) = NULL;
    if (sizeof(version) != sizeof(host_symbol)) return 5;
    memcpy(&version, &host_symbol, sizeof(version));
    if (version() != UINT32_C(1)) return 6;
    for (size_t i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); ++i) {
        if (dlsym(handle, forbidden[i]) != NULL) return 7;
    }
    if (dlclose(handle) != 0) return 8;
    puts("REAL_ORESLANG_C_U32_ABI_VERSION_PASS_NOT_BROWSER_BACKEND");
    return 0;
}
