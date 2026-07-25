/*
 * consumer.c — Phase 1 (v5) pinned-core busy-poll consumer.
 *
 * Change C (v5): no sleep, no eventfd, no condvar. Dedicated thread
 * pinned to CPU N-1. Detects descriptors in nanoseconds (DPDK/BBDEV
 * pattern — the ring is userspace shared memory, no kernel transition).
 *
 * Phase 1: handle() is a STUB — copies llr→out and counts descriptors.
 * Phase 2 wires the real OpenCL kernel in place of the stub.
 *
 * Usage:
 *   consumer [--ring-path <shm>] [--cpu <N>] [--count <K>]
 *   The ring is mapped from a file path so ring_test.c can share it.
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
#include <signal.h>

#include "desc_ring.h"
#include "cxl_region.h"

/* ---- ring shared-memory path ------------------------------------------- */
#define RING_SHM_DEFAULT "/tmp/v5_desc_ring.bin"

/* ---- histogram ---------------------------------------------------------- */
#define HIST_BUCKETS 64
static uint64_t g_hist_ns[HIST_BUCKETS];   /* latency histogram: 2^i ns buckets */
static uint64_t g_descriptors_seen = 0;
static uint64_t g_drops_detected   = 0;

static int hist_bucket(uint64_t ns) {
    if (ns == 0) return 0;
    int b = 63 - __builtin_clzll(ns);
    return (b < HIST_BUCKETS) ? b : (HIST_BUCKETS - 1);
}

static volatile sig_atomic_t g_stop = 0;
static void handle_sig(int s) { (void)s; g_stop = 1; }

/* ---- stub handle -------------------------------------------------------- */
/* Phase 1: memcpy llr→out. Phase 2 replaces this with OpenCL dispatch. */
static void *g_cxl_base = NULL;

static void handle_stub(const struct ldpc_desc *d, uint64_t t_pop_ns)
{
    if (g_cxl_base && d->llr_len && d->out_len) {
        size_t copy_len = d->llr_len < d->out_len ? d->llr_len : d->out_len;
        memcpy((uint8_t *)g_cxl_base + d->out_off,
               (uint8_t *)g_cxl_base + d->llr_off,
               copy_len);
    }

    uint64_t t_now = (uint64_t)({ struct timespec ts;
                                   clock_gettime(CLOCK_MONOTONIC, &ts);
                                   (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec; });
    uint64_t lat_ns = t_now - t_pop_ns;
    g_hist_ns[hist_bucket(lat_ns)]++;
    g_descriptors_seen++;
}

/* ---- busy-poll thread --------------------------------------------------- */

typedef struct {
    desc_ring_t *ring;
    int          cpu;
    uint64_t     target_count;   /* stop after this many (0 = run until signal) */
} poll_args_t;

static void *poll_thread(void *arg)
{
    poll_args_t *a = (poll_args_t *)arg;

    /* Pin to requested CPU */
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(a->cpu, &cs);
    if (pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs) != 0)
        fprintf(stderr, "[consumer] WARNING: could not pin to CPU %d\n", a->cpu);
    else
        fprintf(stderr, "[consumer] pinned poll thread to CPU %d\n", a->cpu);

    uint32_t last_seq = 0;
    int seq_init = 0;

    struct ldpc_desc d;
    while (!g_stop && (a->target_count == 0 || g_descriptors_seen < a->target_count)) {
        if (desc_ring_try_pop(a->ring, &d)) {
            uint64_t t_pop_ns = (uint64_t)({ struct timespec ts;
                                              clock_gettime(CLOCK_MONOTONIC, &ts);
                                              (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec; });

            /* Sequence-gap detection */
            if (seq_init && d.seq != last_seq + 1)
                g_drops_detected += (d.seq - last_seq - 1);
            last_seq = d.seq;
            seq_init = 1;

            handle_stub(&d, t_pop_ns);
        } else {
            /* PAUSE — no syscall, no sleep, no scheduler wakeup */
            __asm__ volatile("pause" ::: "memory");
        }
    }
    return NULL;
}

/* ---- reporting ---------------------------------------------------------- */

static void print_histogram(void)
{
    printf("[consumer] latency histogram (poll-to-handle-complete):\n");
    printf("  bucket_ns_floor  count  pct\n");
    uint64_t total = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) total += g_hist_ns[i];
    if (!total) { printf("  (no samples)\n"); return; }

    uint64_t cumul = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        if (!g_hist_ns[i]) continue;
        uint64_t floor_ns = (i == 0) ? 0 : (1ULL << (i - 1));
        cumul += g_hist_ns[i];
        printf("  %-16llu %8llu  %5.1f%%  (cumul %5.1f%%)\n",
               (unsigned long long)floor_ns,
               (unsigned long long)g_hist_ns[i],
               100.0 * g_hist_ns[i] / total,
               100.0 * cumul / total);
    }

    /* Derive p50, p95, p99 */
    uint64_t p50 = 0, p95 = 0, p99 = 0;
    cumul = 0;
    for (int i = 0; i < HIST_BUCKETS; i++) {
        cumul += g_hist_ns[i];
        uint64_t pct100 = cumul * 100 / total;
        if (!p50 && pct100 >= 50) p50 = (i == 0) ? 0 : (1ULL << (i-1));
        if (!p95 && pct100 >= 95) p95 = (i == 0) ? 0 : (1ULL << (i-1));
        if (!p99 && pct100 >= 99) p99 = (i == 0) ? 0 : (1ULL << (i-1));
    }
    printf("[consumer] p50=%llu ns  p95=%llu ns  p99=%llu ns\n",
           (unsigned long long)p50,
           (unsigned long long)p95,
           (unsigned long long)p99);
}

/* ---- main -------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *ring_path = RING_SHM_DEFAULT;
    int cpu = -1;          /* -1 = auto: use nproc-1 */
    uint64_t target_count = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--ring-path") && i+1 < argc) ring_path = argv[++i];
        else if (!strcmp(argv[i], "--cpu") && i+1 < argc) cpu = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--count") && i+1 < argc) target_count = (uint64_t)atoll(argv[++i]);
    }

    if (cpu < 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cpu = (n > 1) ? (int)(n - 1) : 0;
    }
    fprintf(stderr, "[consumer] ring_path=%s  cpu=%d  target_count=%llu\n",
            ring_path, cpu, (unsigned long long)target_count);

    /* Map the ring */
    int fd = open(ring_path, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    assert(ftruncate(fd, sizeof(desc_ring_t)) == 0);
    desc_ring_t *ring = mmap(NULL, sizeof(desc_ring_t),
                              PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    assert(ring != MAP_FAILED);
    desc_ring_init(ring);
    fprintf(stderr, "[consumer] ring mapped at %p (%zu bytes)\n",
            (void*)ring, sizeof(desc_ring_t));

    /* Map the CXL stand-in for stub handle() */
    g_cxl_base = cxl_region_map(CXL_REGION_SIZE, NULL);

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);

    poll_args_t pa = { .ring = ring, .cpu = cpu, .target_count = target_count };
    pthread_t tid;
    pthread_create(&tid, NULL, poll_thread, &pa);
    fprintf(stderr, "[consumer] poll thread started — waiting for descriptors\n");

    pthread_join(tid, NULL);

    printf("[consumer] descriptors_seen=%llu  drops_detected=%llu\n",
           (unsigned long long)g_descriptors_seen,
           (unsigned long long)g_drops_detected);
    print_histogram();

    cxl_region_unmap(g_cxl_base, CXL_REGION_SIZE);
    munmap(ring, sizeof(desc_ring_t));
    return (g_drops_detected > 0) ? 1 : 0;
}
