#include "oreslang/runtime/NativeI64MapRegion.h"
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static void require(int condition) { if (!condition) abort(); }

static void positives(void) {
    OresNativeI64MapEntry storage[8];
    OresNativeI64MapRegion map;
    oreslang_i64_map_init(&map, storage, 8);

    require(oreslang_i64_map_size(&map) == 0);
    require(oreslang_i64_map_put(&map, 7, 70) == ORES_NATIVE_I64_MAP_OK);
    require(oreslang_i64_map_put(&map, -1, 10) == ORES_NATIVE_I64_MAP_OK);
    require(oreslang_i64_map_put(&map, INT64_MAX, INT64_MIN) == ORES_NATIVE_I64_MAP_OK);
    require(oreslang_i64_map_size(&map) == 3);

    int64_t value = 123456;
    require(oreslang_i64_map_get(&map, 7, &value) == 1 && value == 70);
    value = 123456;
    require(oreslang_i64_map_get(&map, 999, &value) == 0 && value == 123456);

    require(oreslang_i64_map_put(&map, 7, 71) == ORES_NATIVE_I64_MAP_OK);
    require(oreslang_i64_map_size(&map) == 3);
    require(oreslang_i64_map_get(&map, 7, &value) == 1 && value == 71);

    const int64_t expected_keys[3] = {7, -1, INT64_MAX};
    const int64_t expected_values[3] = {71, 10, INT64_MIN};
    for (uint64_t i = 0; i < 3; ++i) {
        int64_t key = 0;
        value = 0;
        require(oreslang_i64_map_entry_at(&map, i, &key, &value) == 1);
        require(key == expected_keys[i] && value == expected_values[i]);
    }
    int64_t key = 88;
    value = 99;
    require(oreslang_i64_map_entry_at(&map, 3, &key, &value) == 0);
    require(key == 88 && value == 99);

    for (int64_t i = 0; i < 5; ++i)
        require(oreslang_i64_map_put(&map, 100 + i, 200 + i) == ORES_NATIVE_I64_MAP_OK);
    require(oreslang_i64_map_size(&map) == 8);
    require(oreslang_i64_map_put(&map, 999, 1) == ORES_NATIVE_I64_MAP_FULL);
    require(oreslang_i64_map_size(&map) == 8);

    oreslang_i64_map_close(&map);
    const unsigned char* bytes = (const unsigned char*)storage;
    for (size_t i = 0; i < sizeof(storage); ++i) require(bytes[i] == 0);
}

#if defined(__unix__) || defined(__APPLE__)
static void must_abort(int which) {
    pid_t pid = fork();
    require(pid >= 0);
    if (pid == 0) {
        struct rlimit limit = {0,0};
        setrlimit(RLIMIT_CORE, &limit);
        OresNativeI64MapEntry storage[2];
        OresNativeI64MapRegion map;
        if (which == 0) {
            oreslang_i64_map_init(NULL, storage, 2);
        } else if (which == 1) {
            oreslang_i64_map_init(&map, NULL, 2);
        } else if (which == 2) {
            oreslang_i64_map_init(&map, storage, 0);
        } else {
            oreslang_i64_map_init(&map, storage, 2);
            if (which == 3) (void)oreslang_i64_map_get(&map, 1, NULL);
            else if (which == 4) (void)oreslang_i64_map_entry_at(&map, 0, NULL, NULL);
            else if (which == 5) { oreslang_i64_map_close(&map); (void)oreslang_i64_map_size(&map); }
            else abort();
        }
        _exit(0);
    }
    int status = 0;
    require(waitpid(pid, &status, 0) == pid);
    require(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}
#endif

int main(void) {
    positives();
#if defined(__unix__) || defined(__APPLE__)
    for (int i = 0; i <= 5; ++i) must_abort(i);
#endif
    return 0;
}
