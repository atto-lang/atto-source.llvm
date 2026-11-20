/*
 * Auditor self-test. This object is deliberately hosted: it reaches for an
 * allocator, stdio, OS threads and the actor runtime. The gate requires its
 * symbol audit to flag every one of them and requires the no-libc link to
 * fail, so a passing gate cannot be a vacuous one.
 */
extern void *malloc(unsigned int size);
extern int printf(const char *format, ...);
extern int pthread_create(void *thread, const void *attr, void *(*start)(void *), void *arg);
extern void ores_actor_spawn(void);

long long ores_core_entry(long long x) {
    void *p = malloc(4u);
    printf("%p", p);
    pthread_create(0, 0, 0, 0);
    ores_actor_spawn();
    return x;
}

long long ores_core_overflow_probe(long long x) {
    return x;
}
