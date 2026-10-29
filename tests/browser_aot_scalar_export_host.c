/* Loads a REAL .ores->LLVM->AOT scalar export: no fake browser backend.
 * This proves only the current i64 guest export ABI, NOT the five browser
 * C ABI v1 exports, actor-state lowering, renderer isolation or Qt.
 */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROBE "ores_browser_aot_scalar_probe"
static const char *const browser_exports[] = {
    "ores_browser_abi_version", "ores_browser_open",
    "ores_browser_dispatch", "ores_browser_snapshot", "ores_browser_close"
};

int main(int argc, char **argv) {
    if (argc != 2 || argv[1][0] != '/') {
        fputs("FAIL:absolute shared object path required\n", stderr);
        return 2;
    }
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fputs("FAIL:cannot load AOT library\n", stderr); return 3; }
    void *symbol = dlsym(lib, PROBE);
    if (!symbol) { fputs("FAIL:missing Oreslang scalar export\n", stderr); return 4; }
    int64_t (*probe)(void) = NULL;
    if (sizeof(probe) != sizeof(symbol)) return 5;
    memcpy(&probe, &symbol, sizeof(probe));
    if (probe() != INT64_C(20261029)) {
        fputs("FAIL:wrong guest scalar result\n", stderr);
        return 6;
    }
    for (size_t i = 0; i < sizeof(browser_exports)/sizeof(browser_exports[0]); ++i) {
        if (dlsym(lib, browser_exports[i]) != NULL) {
            fputs("FAIL:canary illegally claims browser ABI export\n", stderr);
            return 7;
        }
    }
    if (dlclose(lib) != 0) return 8;
    puts("NATIVE_ORESLANG_AOT_SCALAR_EXPORT_PASS_NOT_BROWSER_ABI");
    return 0;
}
