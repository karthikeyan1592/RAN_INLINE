/*
 * ring_test.c — Gate 1 (v5): test producer for the desc_ring + busy-poll consumer.
 *
 * Runs K descriptors through the ring (K = 1e3, 1e5, 1e6 per spec).
 * The consumer process must be running concurrently (start consumer first).
 *
 * For self-contained single-process testing (Gate 1 mode), this binary
 * also spawns its own inline consumer thread so the test is hermetic.
 *
 * Usage:
 *   ring_test --count <K> [--cpu-producer <N>] [--ring-path <path>]
 *             [--self-contained]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <assert.h>

#include "desc_ring.h"
#include "cxl_region.h"

#define RING_SHM_DEFAULT "/tmp/v5_desc_ring.bin"

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

/* ---- inline consumer (self-contained mode) ----------------------------- */

typedef struct {
    desc_ring_t *ring;
    uint64_t     target_count;
    uint64_t     seen;
    uint64_t     drops;
    uint64_t     lat_sum_ns;
    uint64_t     lat_p50_ns;   /* filled at end */
    uint64_t     lat_p95_ns;
    uint64_t     lat_p99_ns;
    uint64_t    *lat_samples;  /* allocated to target_count */
    int          cpu;
    volatile int ready;        /* set to 1 when thread is pinned and polling */
} inline_consumer_t;

static void *inline_consumer_thread(void *arg)
{
    inline_consumer_t *c = (inline_consumer_t *)arg;

    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(c->cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    atomic_store_explicit((_Atomic int*)&c->ready, 1, memory_order_release);

    uint32_t last_seq = 0;
    int seq_init = 0;
    struct ldpc_desc d;

    while (c->seen < c->target_count) {
        if (desc_ring_try_pop(c->ring, &d)) {
            uint64_t t_pop = now_ns();

            if (seq_init && d.seq != last_seq + 1)
                c->drops += (d.seq - last_seq - 1);
            last_seq = d.seq;
            seq_init = 1;

            /* stub: nothing to compute in Phase 1 */
            uint64_t lat = now_ns() - t_pop;
            c->lat_sum_ns += lat;
            if (c->lat_samples)
                c->lat_samples[c->seen] = lat;
            c->seen++;
        } else {
            __asm__ volatile("pause" ::: "memory");
        }
    }
    return NULL;
}

static int u64_cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t*)a, y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

/* ---- producer ---------------------------------------------------------- */

static void run_test(desc_ring_t *ring, uint64_t K, int prod_cpu, int cons_cpu,
                     void *cxl_base __attribute__((unused)))
{
    printf("\n=== ring_test: K=%llu ===\n", (unsigned long long)K);

    /* Allocate sample array */
    uint64_t *samples = malloc(K * sizeof(uint64_t));
    assert(samples);

    /* Start inline consumer */
    inline_consumer_t cons = {
        .ring = ring, .target_count = K, .cpu = cons_cpu,
        .lat_samples = samples
    };
    pthread_t ctid;
    pthread_create(&ctid, NULL, inline_consumer_thread, &cons);

    /* Wait for consumer to be pinned and polling */
    while (!atomic_load_explicit((_Atomic int*)&cons.ready, memory_order_acquire))
        __asm__ volatile("pause" ::: "memory");

    /* Pin producer */
    if (prod_cpu >= 0) {
        cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(prod_cpu, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
    }

    desc_ring_init(ring);

    uint64_t t_start = now_ns();
    uint64_t n_pushed = 0, n_retried = 0;

    for (uint64_t i = 0; i < K; i++) {
        struct ldpc_desc d = {
            .timestamp_ns = now_ns(),
            .slot_id      = (uint32_t)(i / 2),
            .cb_index     = (uint32_t)(i % 2),
            .llr_off      = CXL_LLR_OFFSET,
            .llr_len      = 1024,
            .out_off      = CXL_OUT_OFFSET,
            .out_len      = 128,
            .seq          = (uint32_t)(i + 1),
            .bg           = 1,
            .Zc           = 224,
        };
        /* Spin-retry until space available (never drops on producer side) */
        while (!desc_ring_try_push(ring, &d)) {
            __asm__ volatile("pause" ::: "memory");
            n_retried++;
        }
        n_pushed++;
    }

    uint64_t t_push_done = now_ns();

    pthread_join(ctid, NULL);

    uint64_t t_total = now_ns() - t_start;

    /* Compute percentiles */
    qsort(samples, (size_t)cons.seen, sizeof(uint64_t), u64_cmp);
    uint64_t p50 = samples[(size_t)(cons.seen * 50 / 100)];
    uint64_t p95 = samples[(size_t)(cons.seen * 95 / 100)];
    uint64_t p99 = samples[(size_t)(cons.seen * 99 / 100)];
    uint64_t mean = cons.seen ? cons.lat_sum_ns / cons.seen : 0;

    printf("  pushed:          %llu\n",  (unsigned long long)n_pushed);
    printf("  seen (consumer): %llu\n",  (unsigned long long)cons.seen);
    printf("  drops detected:  %llu\n",  (unsigned long long)cons.drops);
    printf("  retries:         %llu\n",  (unsigned long long)n_retried);
    printf("  push duration:   %llu us\n", (unsigned long long)((t_push_done - t_start) / 1000));
    printf("  total duration:  %llu us\n", (unsigned long long)(t_total / 1000));
    printf("  poll-to-handle latency:  mean=%llu ns  p50=%llu ns  p95=%llu ns  p99=%llu ns\n",
           (unsigned long long)mean,
           (unsigned long long)p50,
           (unsigned long long)p95,
           (unsigned long long)p99);

    int pass = (cons.seen == K) && (cons.drops == 0);
    printf("  GATE1 K=%-8llu %s\n", (unsigned long long)K, pass ? "PASS" : "FAIL");
    if (!pass) {
        fprintf(stderr, "[ring_test] FAIL: seen=%llu expected=%llu drops=%llu\n",
                (unsigned long long)cons.seen,
                (unsigned long long)K,
                (unsigned long long)cons.drops);
        exit(1);
    }

    free(samples);
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *ring_path = RING_SHM_DEFAULT;
    int prod_cpu = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ring-path") && i+1 < argc) ring_path = argv[++i];
        else if (!strcmp(argv[i], "--cpu-producer") && i+1 < argc) prod_cpu = atoi(argv[++i]);
    }

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int cons_cpu = (ncpu > 1) ? (int)(ncpu - 1) : 0;
    if (prod_cpu < 0) prod_cpu = (ncpu > 2) ? (int)(ncpu - 2) : 0;
    fprintf(stderr, "[ring_test] prod_cpu=%d cons_cpu=%d ncpu=%ld ring=%s\n",
            prod_cpu, cons_cpu, ncpu, ring_path);

    /* Map shared ring */
    int fd = open(ring_path, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, sizeof(desc_ring_t)) == 0);
    desc_ring_t *ring = mmap(NULL, sizeof(desc_ring_t),
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    assert(ring != MAP_FAILED);

    /* Map CXL stand-in */
    void *cxl_base = cxl_region_map(CXL_REGION_SIZE, NULL);
    printf("[ring_test] CXL backing: %s  base=%p\n",
           cxl_region_backing_path(), cxl_base);
    printf("[ring_test] Alignment check: %s\n",
           ((uintptr_t)cxl_base % 4096 == 0) ? "4096-ALIGNED OK" : "MISALIGNED FAIL");

    /* Gate 1 spec: K = 1e3, 1e5, 1e6 */
    uint64_t K_vals[] = { 1000, 100000, 1000000 };
    for (int k = 0; k < 3; k++)
        run_test(ring, K_vals[k], prod_cpu, cons_cpu, cxl_base);

    cxl_region_unmap(cxl_base, CXL_REGION_SIZE);
    munmap(ring, sizeof(desc_ring_t));
    printf("\nAll Gate 1 ring tests PASSED.\n");
    return 0;
}
