/* bpftime / kernel-uprobe smoke target.
 * A no-op function called in a tight loop; we measure mean wall-clock
 * latency per call WITH a uprobe attached vs WITHOUT. Same shape as the
 * earlier D1 isolation (100k-iteration no-op call) referenced in v4 0.1.
 */
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <stdlib.h>

#define ITERS 100000

/* noinline + not static so a uprobe can resolve it by symbol name */
__attribute__((noinline)) int target_func(int x) {
    __asm__ __volatile__("" ::: "memory"); /* prevent elision */
    return x + 1;
}

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

int main(int argc, char **argv) {
    int reps = (argc > 1) ? atoi(argv[1]) : 3;
    volatile int sink = 0;
    /* warm up */
    for (int i = 0; i < ITERS; i++) sink += target_func(i);

    for (int r = 0; r < reps; r++) {
        uint64_t t0 = now_ns();
        for (int i = 0; i < ITERS; i++) sink += target_func(i);
        uint64_t t1 = now_ns();
        double per_call = (double)(t1 - t0) / ITERS;
        printf("run %d: %.1f ns/call (total %.3f ms, sink=%d)\n",
               r, per_call, (t1 - t0) / 1e6, sink);
        fflush(stdout);
    }
    return 0;
}
