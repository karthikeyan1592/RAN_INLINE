/* ldpc_consumer_v5.c — Phase 3 (v5): descriptor-only uprobe consumer.
 *
 * Loads ldpc_probe_v5 into bpftime's syscall-server, busy-polls the BPF
 * RINGBUF for descriptors, relays LLR from staging→CXL on WSL2 stand-in,
 * pushes each descriptor through the Phase 1 SPSC desc_ring, and collects
 * thread-ID evidence to resolve the DEV-003 SPSC-vs-MPSC ghost.
 *
 * Run as:
 *   LD_PRELOAD=.../libbpftime-syscall-server.so \
 *     SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf \
 *     ./ldpc_consumer_v5 <N_target>   # default 200
 *
 * Then start gNB in gnb-ns:
 *   ip netns exec gnb-ns env \
 *     LD_PRELOAD=.../libbpftime-agent.so \
 *     LD_LIBRARY_PATH=$OAI_BUILD \
 *     $OAI_BUILD/nr-softmodem -O ... --phy-test --rfsim --noS1 ...
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/mman.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ldpc_probe_v5.skel.h"

#include "desc_ring.h"
#include "cxl_region.h"

#define LLR_MAX_BYTES    (68 * 384)
#define TID_LOG_CAP      100
#define CXL_OUT_OFFSET   (128UL * 1024 * 1024)

/* CXL config struct — must match cxl_cfg_t in ldpc_probe_v5.bpf.c exactly */
struct cxl_cfg_t {
    uint64_t region_base;
    uint64_t region_size;
    uint64_t out_base_off;
    uint32_t is_standin;
    uint32_t _pad;
};

/* ---- globals ---- */
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void       *g_cxl_base     = NULL;
static size_t      g_cxl_size     = 0;
static int         g_is_standin   = 1;
static int         g_llr_stage_fd = -1;

static desc_ring_t *g_ring = NULL;

static uint64_t g_desc_count  = 0;
static uint64_t g_ring_drops  = 0;
static uint64_t g_first_ts_ns = 0;
static uint64_t g_last_ts_ns  = 0;

/* Staging buffer for WSL2 LLR relay (one copy: staging→CXL stand-in) */
static uint8_t g_staging_buf[LLR_MAX_BYTES];

/* ---- ring_buffer callback ---- */
static int handle_desc(void *ctx, void *data, size_t sz)
{
    (void)ctx;
    if (sz < sizeof(struct ldpc_desc)) return 0;

    struct ldpc_desc *d = (struct ldpc_desc *)data;

    if (!g_first_ts_ns) g_first_ts_ns = d->timestamp_ns;
    g_last_ts_ns = d->timestamp_ns;

    /* WSL2 stand-in: relay LLR from staging map → CXL region.
     * On DO: skipped entirely (llr_off already points into /dev/dax0.0). */
    if (g_is_standin && g_cxl_base && g_llr_stage_fd >= 0) {
        uint32_t key = 0;
        if (bpf_map_lookup_elem(g_llr_stage_fd, &key, g_staging_buf) == 0) {
            uint32_t copy_len = d->llr_len < LLR_MAX_BYTES
                                ? d->llr_len : (uint32_t)LLR_MAX_BYTES;
            if (d->llr_off + copy_len <= g_cxl_size)
                memcpy((char *)g_cxl_base + d->llr_off,
                       g_staging_buf, copy_len);
        }
    }

    /* Push into SPSC desc_ring (Phase 1 integration exercise) */
    int pushed = 0;
    if (g_ring)
        pushed = desc_ring_try_push(g_ring, d);

    if (!pushed)
        g_ring_drops++;
    else
        g_desc_count++;

    /* Drain desc_ring inline so it never fills (Phase 3: no GPU handler yet) */
    if (g_ring) {
        struct ldpc_desc popped;
        while (desc_ring_try_pop(g_ring, &popped)) { /* discard */ }
    }

    return 0;
}

/* ---- unique-TID counter ---- */
static int count_unique(const uint64_t *tids, int n)
{
    uint64_t seen[TID_LOG_CAP];
    int nu = 0;
    for (int i = 0; i < n; i++) {
        int dup = 0;
        for (int j = 0; j < nu; j++) {
            if (seen[j] == tids[i]) { dup = 1; break; }
        }
        if (!dup) seen[nu++] = tids[i];
    }
    return nu;
}

/* ---- main ---- */
int main(int argc, char **argv)
{
    int n_target = (argc > 1) ? atoi(argv[1]) : 200;
    fprintf(stderr, "[consumer_v5] target=%d descriptors\n", n_target);

    signal(SIGINT,  on_sig);
    signal(SIGTERM, on_sig);

    /* 1. Map CXL stand-in (or real DAX on DO) */
    g_cxl_base  = cxl_region_map(CXL_REGION_SIZE, NULL);
    g_cxl_size  = CXL_REGION_SIZE;
    const char *backing = cxl_region_backing_path();
    g_is_standin = (strncmp(backing, "/dev/dax", 8) != 0);
    fprintf(stderr, "[consumer_v5] CXL backing=%s  base=%p  standin=%d\n",
            backing, g_cxl_base, g_is_standin);

    /* 2. Create / open SPSC desc_ring in shared memory */
    const char *ring_path = "/tmp/v5_desc_ring.bin";
    int ring_fd = open(ring_path, O_RDWR | O_CREAT, 0600);
    if (ring_fd < 0) { perror("open ring"); return 1; }
    if (ftruncate(ring_fd, (off_t)sizeof(desc_ring_t)) != 0) {
        perror("ftruncate ring"); return 1;
    }
    g_ring = mmap(NULL, sizeof(desc_ring_t), PROT_READ | PROT_WRITE,
                  MAP_SHARED, ring_fd, 0);
    close(ring_fd);
    if (g_ring == MAP_FAILED) { perror("mmap ring"); return 1; }
    desc_ring_init(g_ring);

    /* 3. Load BPF skeleton (bpftime syscall-server intercepts bpf() calls) */
    struct ldpc_probe_v5 *skel = ldpc_probe_v5__open();
    if (!skel) { fprintf(stderr, "[consumer_v5] BPF open failed\n"); return 1; }
    if (ldpc_probe_v5__load(skel)) {
        fprintf(stderr, "[consumer_v5] BPF load failed\n"); return 1;
    }

    /* 4. Write CXL config into BPF map before probe fires */
    struct cxl_cfg_t cfg = {
        .region_base  = (uint64_t)(uintptr_t)g_cxl_base,
        .region_size  = (uint64_t)g_cxl_size,
        .out_base_off = (uint64_t)CXL_OUT_OFFSET,
        .is_standin   = g_is_standin ? 1u : 0u,
        ._pad         = 0u,
    };
    uint32_t k = 0;
    int cxl_cfg_fd = bpf_map__fd(skel->maps.cxl_config);
    bpf_map_update_elem(cxl_cfg_fd, &k, &cfg, BPF_ANY);
    fprintf(stderr, "[consumer_v5] cxl_config: is_standin=%u  base=0x%lx\n",
            cfg.is_standin, (unsigned long)cfg.region_base);

    /* 5. Save staging map fd for WSL2 relay in callback */
    g_llr_stage_fd = bpf_map__fd(skel->maps.llr_staging);

    /* 6. Attach probes */
    if (ldpc_probe_v5__attach(skel)) {
        fprintf(stderr, "[consumer_v5] BPF attach failed\n");
        ldpc_probe_v5__destroy(skel);
        return 1;
    }
    fprintf(stderr, "[consumer_v5] probes attached — busy-polling for %d CBs...\n",
            n_target);

    /* 7. Create ring_buffer consumer on BPF RINGBUF */
    struct ring_buffer *rb = ring_buffer__new(
        bpf_map__fd(skel->maps.desc_ringbuf),
        handle_desc, NULL, NULL);
    if (!rb) {
        fprintf(stderr, "[consumer_v5] ring_buffer__new failed\n");
        ldpc_probe_v5__destroy(skel);
        return 1;
    }

    /* 8. Busy-poll loop (Change C: no sleep, no epoll) */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    while (!g_stop && (int64_t)g_desc_count < (int64_t)n_target) {
        ring_buffer__consume(rb);           /* drains available records */
        __asm__ volatile("pause" ::: "memory");  /* x86 spin hint */
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1e3
                      + (t1.tv_nsec - t0.tv_nsec) * 1e-6;

    /* 9. Read TID evidence from BPF maps */
    uint32_t n_tid_logged = 0;
    bpf_map_lookup_elem(bpf_map__fd(skel->maps.tid_count),
                        &k, &n_tid_logged);
    int n_to_read = (int)n_tid_logged < TID_LOG_CAP
                    ? (int)n_tid_logged : TID_LOG_CAP;

    uint64_t tids[TID_LOG_CAP] = {};
    int tid_fd = bpf_map__fd(skel->maps.tid_log);
    for (int i = 0; i < n_to_read; i++) {
        uint32_t ki = (uint32_t)i;
        bpf_map_lookup_elem(tid_fd, &ki, &tids[i]);
    }
    int n_unique = count_unique(tids, n_to_read);

    /* 10. Print Gate 3 report */
    fprintf(stderr, "\n[consumer_v5] === GATE 3 REPORT ===\n");
    fprintf(stderr, "desc_count:      %lu\n",  g_desc_count);
    fprintf(stderr, "ring_drops:      %lu\n",  g_ring_drops);
    fprintf(stderr, "elapsed_ms:      %.1f\n", elapsed_ms);
    if (g_desc_count > 0 && elapsed_ms > 0)
        fprintf(stderr, "rate_desc_s:     %.1f\n",
                g_desc_count / (elapsed_ms / 1000.0));
    fprintf(stderr, "tid_logged:      %u\n",   n_tid_logged);
    fprintf(stderr, "tid_unique:      %d\n",   n_unique);

    fprintf(stderr, "tid_list:        ");
    for (int i = 0; i < n_to_read && i < 20; i++)
        fprintf(stderr, "%lu ", (unsigned long)tids[i]);
    fprintf(stderr, "\n");

    if (n_unique == 1)
        fprintf(stderr, "spsc_verdict:    SPSC (single producer; ring is safe)\n");
    else if (n_unique > 1)
        fprintf(stderr, "spsc_verdict:    MPSC (%d threads; BPF RINGBUF is the "
                "MPSC layer; relay→desc_ring is single-producer)\n", n_unique);
    else
        fprintf(stderr, "spsc_verdict:    UNKNOWN (insufficient TID sample)\n");

    ring_buffer__free(rb);
    ldpc_probe_v5__destroy(skel);
    return (int64_t)g_desc_count >= (int64_t)n_target ? 0 : 1;
}
