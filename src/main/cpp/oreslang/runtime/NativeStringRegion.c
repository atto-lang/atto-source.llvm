/* Synchronous guest-call lifetime, deterministic destruction, no tracing GC.
 * Handles are typed compiler values, not guest integers or native pointers.
 * Actor/continuation transfer is intentionally not part of this ABI yet.
 */
#define _POSIX_C_SOURCE 200809L
#ifdef __APPLE__
#define _DARWIN_C_SOURCE 1
#endif
#include "NativeStringRegion.h"
#include "NativeStringRegionInternal.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

enum { STRING_LIMIT = 65536, BYTE_LIMIT = 64 * 1024 * 1024, OUTPUT_SLOTS = 1024 };
typedef struct { const char *data; size_t size; char *owned; } String;
typedef struct { String value; int newline; } Output;
typedef struct {
    String *strings;
    size_t count, capacity, bytes;
    int active, console, fd, original_flags, wake[2];
    pthread_t writer;
    Output output[OUTPUT_SLOTS];
    _Atomic size_t head, tail;
    _Atomic int failed, done;
} Region;
static _Thread_local Region region;

static int64_t milliseconds(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}
static int write_ready(Region *r, const char *data, size_t size) {
    const int64_t start = milliseconds();
    if (start < 0) return 0;
    while (size) {
        ssize_t written = write(r->fd, data, size);
        if (written > 0) { data += written; size -= (size_t)written; continue; }
        if (written < 0 && errno == EINTR) continue;
        if (written >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return 0;
        const int64_t now = milliseconds();
        if (now < 0 || now - start >= 5000) return 0;
        struct pollfd descriptor = {r->fd, POLLOUT, 0};
        int result = poll(&descriptor, 1, 100);
        if (result < 0 && errno != EINTR) return 0;
        if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) return 0;
    }
    return 1;
}
static void *output_reactor(void *argument) {
    Region *r = argument;
    sigset_t blocked;
    sigemptyset(&blocked); sigaddset(&blocked, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &blocked, NULL)) { atomic_store(&r->failed, 1); return NULL; }
    for (;;) {
        size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
        size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
        if (tail != head) {
            Output item = r->output[tail % OUTPUT_SLOTS];
            if (!write_ready(r, item.value.data, item.value.size) ||
                    (item.newline && !write_ready(r, "\n", 1))) {
                atomic_store(&r->failed, 1); return NULL;
            }
            atomic_store_explicit(&r->tail, tail + 1, memory_order_release);
        } else {
            if (atomic_load_explicit(&r->done, memory_order_acquire)) {
                /* Acquire done before the final head read: an earlier empty
                 * snapshot must not discard the producer's final record. */
                if (tail == atomic_load_explicit(&r->head, memory_order_acquire)) return NULL;
                continue;
            }
            struct pollfd descriptor = {r->wake[0], POLLIN, 0};
            if (poll(&descriptor, 1, 100) < 0 && errno != EINTR) {
                atomic_store(&r->failed, 1); return NULL;
            }
            char bytes[64];
            while (read(r->wake[0], bytes, sizeof bytes) > 0) {}
        }
    }
}
static void wake_writer(Region *r) {
    /* Full wake pipe already represents readiness; the queue is authoritative. */
    char byte = 1;
    ssize_t written = write(r->wake[1], &byte, 1);
    if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        atomic_store(&r->failed, 1);
}
int64_t ores_string_region_begin(int64_t console) {
    Region *r = &region;
    if (r->active) return 1;
    memset(r, 0, sizeof *r);
    atomic_init(&r->head, 0); atomic_init(&r->tail, 0);
    atomic_init(&r->failed, 0); atomic_init(&r->done, 0);
    r->fd = r->wake[0] = r->wake[1] = -1;
    if (console) {
        struct stat status;
        /* Regular-file writes cannot promise strict NIO, including on macOS. */
        if (fstat(STDOUT_FILENO, &status)) return 1;
        if (!S_ISFIFO(status.st_mode) && !S_ISSOCK(status.st_mode) &&
                !(S_ISCHR(status.st_mode) && isatty(STDOUT_FILENO))) return 1;
        r->fd = dup(STDOUT_FILENO);
        if (r->fd < 0) return 1;
        r->original_flags = fcntl(r->fd, F_GETFL);
        if (r->original_flags < 0) { close(r->fd); return 1; }
        if (fcntl(r->fd, F_SETFL, r->original_flags | O_NONBLOCK)) { close(r->fd); return 1; }
#ifdef F_SETNOSIGPIPE
        /* Darwin's per-descriptor protection also covers pipes; do not alter
         * the embedding process's global SIGPIPE disposition. */
        if (fcntl(r->fd, F_SETNOSIGPIPE, 1)) goto fail;
#endif
        if (pipe(r->wake)) goto fail;
        for (int i = 0; i != 2; ++i) {
            int flags = fcntl(r->wake[i], F_GETFL);
            if (flags < 0 || fcntl(r->wake[i], F_SETFL, flags | O_NONBLOCK)) goto fail;
            if (fcntl(r->wake[i], F_SETFD, FD_CLOEXEC)) goto fail;
        }
        if (fcntl(r->fd, F_SETFD, FD_CLOEXEC)) goto fail;
#ifdef F_SETNOSIGPIPE
        if (fcntl(r->wake[1], F_SETNOSIGPIPE, 1)) goto fail;
#endif
        if (pthread_create(&r->writer, NULL, output_reactor, r)) goto fail;
        r->console = 1;
    }
    r->active = 1;
    return 0;
fail:
    for (int i = 0; i != 2; ++i) if (r->wake[i] >= 0) close(r->wake[i]);
    (void)fcntl(r->fd, F_SETFL, r->original_flags); close(r->fd);
    return 1;
}
static int64_t publish(String value) {
    Region *r = &region;
    if (!r->active || atomic_load(&r->failed) || r->count == STRING_LIMIT || value.size > BYTE_LIMIT - r->bytes) goto fail;
    if (r->count == r->capacity) {
        size_t capacity = r->capacity ? r->capacity * 2 : 32;
        String *strings = realloc(r->strings, capacity * sizeof *strings);
        if (!strings) goto fail;
        r->strings = strings; r->capacity = capacity;
    }
    r->strings[r->count++] = value;
    r->bytes += value.size;
    return (int64_t)r->count;
fail:
    free(value.owned); atomic_store(&r->failed, 1); return 0;
}
static String lookup(int64_t handle) {
    Region *r = &region;
    if (!r->active || handle <= 0 || (uint64_t)handle > r->count) {
        atomic_store(&r->failed, 1); return (String){NULL, 0, NULL};
    }
    return r->strings[handle - 1];
}
int64_t ores_string_literal(const char *data, int64_t size) {
    if (size < 0 || (size && !data)) { atomic_store(&region.failed, 1); return 0; }
    return publish((String){data, (size_t)size, NULL});
}
int64_t ores_string_concat(int64_t left, int64_t right) {
    String a = lookup(left), b = lookup(right);
    if (atomic_load(&region.failed)) return 0;
    if (a.size > BYTE_LIMIT || b.size > BYTE_LIMIT - a.size || a.size + b.size > BYTE_LIMIT - region.bytes) {
        atomic_store(&region.failed, 1); return 0;
    }
    size_t size = a.size + b.size;
    char *bytes = malloc(size ? size : 1);
    if (!bytes) { atomic_store(&region.failed, 1); return 0; }
    if (a.size) memcpy(bytes, a.data, a.size);
    if (b.size) memcpy(bytes + a.size, b.data, b.size);
    return publish((String){bytes, size, bytes});
}
/* Decimal rendering of a signed 64-bit int, as Java prints a long. No libc
 * formatting so INT64_MIN and locale cannot change the bytes. */
int64_t ores_string_from_i64(int64_t value) {
    char digits[20];
    size_t count = 0;
    uint64_t magnitude = value < 0 ? (uint64_t)0 - (uint64_t)value : (uint64_t)value;
    do { digits[count++] = (char)('0' + (int)(magnitude % 10u)); magnitude /= 10u; } while (magnitude);
    size_t size = count + (value < 0);
    char *bytes = malloc(size);
    if (!bytes) { atomic_store(&region.failed, 1); return 0; }
    size_t at = 0;
    if (value < 0) bytes[at++] = '-';
    while (count) bytes[at++] = digits[--count];
    return publish((String){bytes, size, bytes});
}
int64_t ores_string_from_bool(int64_t value) {
    return value ? ores_string_literal("true", 4) : ores_string_literal("false", 5);
}
int64_t ores_string_equal(int64_t left, int64_t right) {
    String a = lookup(left), b = lookup(right);
    if (atomic_load(&region.failed)) return 0;
    return a.size == b.size && (!a.size || memcmp(a.data, b.data, a.size) == 0);
}
/* Immutable length-delimited substring predicate, including embedded NUL.
 * No malloc, pointer escape, mutation or implicit zero-terminator semantics.
 * Empty needle matches, mirroring ordinary String contains semantics.
 */
int64_t ores_string_contains_literal(int64_t haystack, int64_t needle) {
    String text = lookup(haystack), pattern = lookup(needle);
    if (atomic_load(&region.failed)) return 0;
    if (pattern.size == 0) return 1;
    if (pattern.size > text.size) return 0;
    size_t start = 0;
    const size_t last = text.size - pattern.size;
    while (start <= last) {
        const char *at = memchr(text.data + start,
            (unsigned char)pattern.data[0], last - start + 1);
        if (!at) return 0;
        const size_t found = (size_t)(at - text.data);
        if (memcmp(at, pattern.data, pattern.size) == 0) return 1;
        start = found + 1;
    }
    return 0;
}
/* Private, immediate-use read-only borrow. Neither pointer nor length
 * is exposed through any guest function signature. No peer thread may call
 * into this invocation-owned String region concurrently.
 */
int ores_string_region_view(int64_t handle, const char **bytes, size_t *length) {
    if (!bytes || !length) abort();
    String value = lookup(handle);
    if (atomic_load(&region.failed)) return 0;
    *bytes = value.data;
    *length = value.size;
    return 1;
}

/* All-or-nothing region admission for split-created String views. The
 * underlying owned allocation is retained by the original source entry
 * until invocation teardown, even if the metadata vector reallocates.
 * Slice records have owned=NULL, therefore never double-free source bytes.
 */
OresStringViewsStatus ores_string_region_publish_views(
        int64_t source, const size_t starts[], const size_t lengths[],
        size_t count, int64_t out_handles[]) {
    if (!starts || !lengths || !out_handles || !count) abort();
    Region *r = &region;
    String base = lookup(source);
    if (atomic_load(&r->failed)) abort();
    if (count > STRING_LIMIT - r->count) return ORES_STRING_VIEWS_LIMIT;
    size_t remaining = BYTE_LIMIT - r->bytes;
    for (size_t i = 0; i < count; ++i) {
        if (starts[i] > base.size || lengths[i] > base.size - starts[i])
            abort(); /* Only trusted splitter code constructs these offsets. */
        if (lengths[i] > remaining) return ORES_STRING_VIEWS_LIMIT;
        remaining -= lengths[i];
    }
    const size_t required = r->count + count;
    if (required > r->capacity) {
        size_t capacity = r->capacity ? r->capacity : 32;
        while (capacity < required) capacity *= 2;
        String *larger = realloc(r->strings, capacity * sizeof *larger);
        if (!larger) return ORES_STRING_VIEWS_NO_MEMORY;
        r->strings = larger;
        r->capacity = capacity;
    }
    /* No allocation or recoverable failure is possible beyond this point. */
    for (size_t i = 0; i < count; ++i) {
        const char *slice = base.data ? base.data + starts[i] : NULL;
        r->strings[r->count] = (String){slice, lengths[i], NULL};
        out_handles[i] = (int64_t)++r->count;
        r->bytes += lengths[i];
    }
    return ORES_STRING_VIEWS_OK;
}

int64_t ores_string_print(int64_t handle, int64_t newline) {
    Region *r = &region;
    String value = lookup(handle);
    size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    if (!r->console || atomic_load(&r->failed) ||
            head - atomic_load_explicit(&r->tail, memory_order_acquire) == OUTPUT_SLOTS) {
        atomic_store(&r->failed, 1); return 1;
    }
    r->output[head % OUTPUT_SLOTS] = (Output){value, newline != 0};
    atomic_store_explicit(&r->head, head + 1, memory_order_release);
    wake_writer(r);
    return 0;
}
/* A failed unwrap/expect aborts without reaching region_end, which would
 * otherwise drop output still queued for the writer thread. Drain it first so
 * text printed before the panic is not lost (Java flushes stdout per write). */
void ores_string_panic_flush(void) {
    Region *r = &region;
    if (!r->active || !r->console) return;
    atomic_store_explicit(&r->done, 1, memory_order_release);
    wake_writer(r);
    (void)pthread_join(r->writer, NULL);
    r->console = 0;
}
int64_t ores_string_region_end(void) {
    Region *r = &region;
    if (!r->active) return 1;
    if (r->console) {
        atomic_store_explicit(&r->done, 1, memory_order_release);
        wake_writer(r);
        /* Join occurs in the OS wrapper after guest code, before region drop. */
        if (pthread_join(r->writer, NULL)) abort();
        if (fcntl(r->fd, F_SETFL, r->original_flags)) atomic_store(&r->failed, 1);
        close(r->fd); close(r->wake[0]); close(r->wake[1]);
    }
    for (size_t i = 0; i != r->count; ++i) free(r->strings[i].owned);
    free(r->strings); r->strings = NULL; r->active = 0;
    return atomic_load(&r->failed) ? 1 : 0;
}
