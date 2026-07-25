/* ablation.c — Phase 4 (v5) ablation harness.
 *
 * Measures three rows for latency_ladder_v2.csv:
 *   baseline:          PRIMARY_CONFIG anchor 11,703 us/slot — FIXED, not re-measured.
 *   interception_only: descriptor + busy-poll, consumer returns immediately (no OCL).
 *                      MUST be sub-10us total; proves Change C (busy-poll) works.
 *   gpu_compute_full:  same + OpenCL LDPC decode over CXL stand-in.
 *
 * Timing per CB:
 *   T0 = d->timestamp_ns (bpf_ktime_get_ns() at uprobe fire — CLOCK_MONOTONIC)
 *   T1 = clock_gettime(CLOCK_MONOTONIC) at callback entry
 *   T2 = clock_gettime(CLOCK_MONOTONIC) after processing done
 *   total_ns = T2 - T0
 *
 * C=24 projection (DEV-009 resolution):
 *   proj_us_per_slot = measured_mean_us_per_slot * (24.0 / C_actual)
 *
 * Usage:
 *   LD_PRELOAD=.../libbpftime-syscall-server.so SPDLOG_LEVEL=warn BPFTIME_VM_NAME=ubpf \
 *     ./ablation [--mode interception_only|gpu_compute_full] [--n-cbs N] [--cl-path PATH]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <math.h>
#include <sys/mman.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ldpc_probe_v5.skel.h"

#include "desc_ring.h"
#include "cxl_region.h"

/* OpenCL */
#include <CL/cl.h>
#include "../gpu_daemon/ldpc_cl/bg_tables.h"

/* BG constants */
#define N_VN_INFO_BG1  22
#define N_VN_INFO_BG2  10
#define N_VN_FULL_BG1  68
#define N_VN_FULL_BG2  52
#define N_CN_BG1       46
#define N_CN_BG2       42
#define N_ITER         6

#define LLR_MAX_BYTES  (N_VN_FULL_BG1 * 384)
#define CXL_OUT_OFFSET (128UL * 1024 * 1024)
#define MAX_SAMPLES    65536
#define PRIMARY_CONFIG_US  11703.0
#define PRIMARY_CONFIG_C   24

struct cxl_cfg_t {
    uint64_t region_base;
    uint64_t region_size;
    uint64_t out_base_off;
    uint32_t is_standin;
    uint32_t _pad;
};

typedef enum { MODE_INTERCEPT_ONLY, MODE_GPU_FULL } ablation_mode_t;

/* ---- globals ---- */
static volatile sig_atomic_t g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static void    *g_cxl_base     = NULL;
static size_t   g_cxl_size     = 0;
static int      g_is_standin   = 1;
static int      g_llr_stage_fd = -1;
static uint8_t  g_staging_buf[LLR_MAX_BYTES];

static cl_context       ocl_ctx;
static cl_device_id     ocl_dev;
static cl_program       ocl_prog;
static cl_kernel        ocl_kern;
static cl_command_queue ocl_q;
static int              ocl_ready = 0;

static void ocl_die(cl_int e, const char *w)
{
    fprintf(stderr, "[ablation] OCL error %d at %s\n", e, w); exit(1);
}
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "[ablation] Cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    size_t nr = fread(buf, 1, sz, f); buf[nr] = '\0'; fclose(f); return buf;
}
static void ocl_init(const char *cl_path)
{
    cl_int err;
    cl_uint np = 0; clGetPlatformIDs(0, NULL, &np);
    cl_platform_id *plats = malloc(np * sizeof(*plats));
    clGetPlatformIDs(np, plats, NULL);
    cl_uint nd = 0; clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, 0, NULL, &nd);
    cl_device_id *devs = malloc(nd * sizeof(*devs));
    clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, nd, devs, NULL);
    ocl_dev = devs[0]; free(devs); free(plats);

    char dn[128] = ""; clGetDeviceInfo(ocl_dev, CL_DEVICE_NAME, sizeof(dn), dn, NULL);
    fprintf(stderr, "[ablation] OCL device: %s\n", dn);

    ocl_ctx = clCreateContext(NULL, 1, &ocl_dev, NULL, NULL, &err);
    if (err) ocl_die(err, "clCreateContext");
    ocl_q = clCreateCommandQueue(ocl_ctx, ocl_dev, 0, &err);
    if (err) ocl_die(err, "clCreateCommandQueue");

    char *src = read_file(cl_path);
    ocl_prog = clCreateProgramWithSource(ocl_ctx, 1, (const char **)&src, NULL, &err);
    free(src); if (err) ocl_die(err, "clCreateProgramWithSource");

    err = clBuildProgram(ocl_prog, 1, &ocl_dev, "-D NO_EDGE=0xffff -D MAX_LS=384", NULL, NULL);
    if (err) {
        size_t ls = 0;
        clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &ls);
        char *log = malloc(ls + 1);
        clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, ls, log, NULL);
        log[ls] = '\0';
        fprintf(stderr, "OCL build error:\n%s\n", log); free(log); exit(1);
    }
    ocl_kern = clCreateKernel(ocl_prog, "ldpc_decode", &err);
    if (err) ocl_die(err, "clCreateKernel");
    ocl_ready = 1;
}

/* Returns popcount of decoded output, or -1 on error. */
static int ocl_decode_cb(uint8_t bg, uint16_t z, size_t llr_off, size_t out_off)
{
    cl_int err;
    int n_vn_full = (bg == 1) ? N_VN_FULL_BG1 : N_VN_FULL_BG2;
    int n_cn      = (bg == 1) ? N_CN_BG1      : N_CN_BG2;
    int n_vn_info = (bg == 1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;

    int ls_idx = (z <= 384) ? (int)(unsigned char)LS_TO_IDX[z] : 0;
    if (ls_idx == 255) ls_idx = 0;

    const uint16_t *shifts = (bg == 1)
        ? (const uint16_t *)BG1_SHIFTS[ls_idx]
        : (const uint16_t *)BG2_SHIFTS[ls_idx];

    size_t llr_bytes   = (size_t)n_vn_full * z;
    size_t out_bytes   = (size_t)((n_vn_info * z + 7) / 8);
    size_t c2v_bytes   = (size_t)n_cn * n_vn_full * z;
    size_t shift_bytes = (size_t)n_vn_full * n_cn * sizeof(uint16_t);

    if (llr_off + llr_bytes > g_cxl_size || out_off + out_bytes > g_cxl_size) return -1;

    cl_mem cl_llr    = clCreateBuffer(ocl_ctx, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                      llr_bytes, (char *)g_cxl_base + llr_off, &err);
    if (err) return -1;
    cl_mem cl_out    = clCreateBuffer(ocl_ctx, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                      out_bytes, (char *)g_cxl_base + out_off, &err);
    if (err) { clReleaseMemObject(cl_llr); return -1; }
    cl_mem cl_shifts = clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      shift_bytes, (void *)shifts, &err);
    if (err) { clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out); return -1; }
    cl_mem cl_c2v    = clCreateBuffer(ocl_ctx, CL_MEM_READ_WRITE, c2v_bytes, NULL, &err);
    if (err) {
        clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out);
        clReleaseMemObject(cl_shifts); return -1;
    }
    static const uint8_t zero_byte = 0;
    clEnqueueFillBuffer(ocl_q, cl_c2v, &zero_byte, 1, 0, c2v_bytes, 0, NULL, NULL);

    cl_int n_vn_full_a = n_vn_full, n_cn_a = n_cn, n_vn_info_a = n_vn_info;
    cl_int ls_a = (cl_int)z, n_iter_a = N_ITER, cb_off_a = 0;
    clSetKernelArg(ocl_kern, 0, sizeof(cl_mem), &cl_llr);
    clSetKernelArg(ocl_kern, 1, sizeof(cl_mem), &cl_out);
    clSetKernelArg(ocl_kern, 2, sizeof(cl_mem), &cl_shifts);
    clSetKernelArg(ocl_kern, 3, sizeof(cl_mem), &cl_c2v);
    clSetKernelArg(ocl_kern, 4, sizeof(cl_int), &n_vn_full_a);
    clSetKernelArg(ocl_kern, 5, sizeof(cl_int), &n_cn_a);
    clSetKernelArg(ocl_kern, 6, sizeof(cl_int), &n_vn_info_a);
    clSetKernelArg(ocl_kern, 7, sizeof(cl_int), &ls_a);
    clSetKernelArg(ocl_kern, 8, sizeof(cl_int), &n_iter_a);
    clSetKernelArg(ocl_kern, 9, sizeof(cl_int), &cb_off_a);

    size_t gws = (size_t)n_cn;
    clEnqueueNDRangeKernel(ocl_q, ocl_kern, 1, NULL, &gws, NULL, 0, NULL, NULL);
    clFinish(ocl_q);

    uint8_t *out_buf = (uint8_t *)g_cxl_base + out_off;
    int ones = 0;
    for (size_t i = 0; i < out_bytes; i++)
        ones += __builtin_popcount(out_buf[i]);

    clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out);
    clReleaseMemObject(cl_shifts); clReleaseMemObject(cl_c2v);
    return ones;
}

/* ---- per-mode run state ---- */
typedef struct {
    ablation_mode_t mode;
    int             n_target;
    uint64_t        n_received;
    uint64_t        n_ocl_ones;
    uint64_t        n_ocl_bits;
    uint64_t       *samples_ns;
    int             n_samples;
    uint64_t        t_start_ns;
    uint64_t        t_first_desc_ns;
    uint64_t        t_last_desc_ns;
    uint64_t        last_ts_ns;
    int             cb_this_slot;
    int             cb_per_slot_max;
} run_state_t;
static run_state_t g_rs;

static int handle_desc(void *ctx, void *data, size_t sz)
{
    (void)ctx;
    if (sz < sizeof(struct ldpc_desc)) return 0;
    struct ldpc_desc *d = (struct ldpc_desc *)data;

    struct timespec ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts1);

    /* WSL2 relay: staging map → CXL backing */
    if (g_is_standin && g_llr_stage_fd >= 0 && g_cxl_base) {
        uint32_t key = 0;
        if (bpf_map_lookup_elem(g_llr_stage_fd, &key, g_staging_buf) == 0) {
            uint32_t copy_len = (d->llr_len < LLR_MAX_BYTES)
                                ? d->llr_len : (uint32_t)LLR_MAX_BYTES;
            if (d->llr_off + copy_len <= g_cxl_size)
                memcpy((char *)g_cxl_base + d->llr_off, g_staging_buf, copy_len);
        }
    }

    /* OpenCL decode (gpu_compute_full) */
    if (g_rs.mode == MODE_GPU_FULL && ocl_ready && g_cxl_base) {
        int ones = ocl_decode_cb(d->bg, d->Zc, (size_t)d->llr_off, (size_t)d->out_off);
        if (ones >= 0) {
            g_rs.n_ocl_ones += (uint64_t)ones;
            int n_vn_info = (d->bg == 1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;
            g_rs.n_ocl_bits += (uint64_t)((n_vn_info * d->Zc + 7) / 8) * 8;
        }
    }

    struct timespec ts2;
    clock_gettime(CLOCK_MONOTONIC, &ts2);

    uint64_t t0_ns    = d->timestamp_ns;
    uint64_t t2_ns    = (uint64_t)ts2.tv_sec * 1000000000ULL + ts2.tv_nsec;
    uint64_t total_ns = (t2_ns > t0_ns) ? (t2_ns - t0_ns) : 0;

    if (g_rs.n_samples < MAX_SAMPLES)
        g_rs.samples_ns[g_rs.n_samples++] = total_ns;

    if (!g_rs.t_first_desc_ns) g_rs.t_first_desc_ns = t0_ns;
    g_rs.t_last_desc_ns = t0_ns;

    /* Infer C_actual from inter-CB gap */
    uint64_t gap_ns = (t0_ns > g_rs.last_ts_ns) ? (t0_ns - g_rs.last_ts_ns) : 0;
    if (g_rs.last_ts_ns && gap_ns > 5000000ULL) {
        if (g_rs.cb_this_slot > g_rs.cb_per_slot_max)
            g_rs.cb_per_slot_max = g_rs.cb_this_slot;
        g_rs.cb_this_slot = 1;
    } else {
        g_rs.cb_this_slot++;
    }
    g_rs.last_ts_ns = t0_ns;
    g_rs.n_received++;
    return 0;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}
static double pct_us(uint64_t *s, int n, double p) {
    if (n <= 0) return 0.0;
    qsort(s, n, sizeof(*s), cmp_u64);
    int idx = (int)(p * n / 100.0); if (idx >= n) idx = n - 1;
    return (double)s[idx] / 1000.0;
}
static double mean_us_fn(const uint64_t *s, int n) {
    if (n <= 0) return 0.0;
    double sum = 0.0; for (int i = 0; i < n; i++) sum += (double)s[i];
    return sum / n / 1000.0;
}

int main(int argc, char **argv)
{
    ablation_mode_t mode      = MODE_INTERCEPT_ONLY;
    int             n_target  = 2000;
    int             c_override = 0;   /* 0 = infer; >0 = forced value */
    const char     *cl_path   = "../gpu_daemon/ldpc_cl/ldpc_decode.cl";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            i++;
            mode = !strcmp(argv[i], "gpu_compute_full") ? MODE_GPU_FULL : MODE_INTERCEPT_ONLY;
        } else if (!strcmp(argv[i], "--n-cbs") && i + 1 < argc) {
            n_target = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--c-actual") && i + 1 < argc) {
            c_override = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--cl-path") && i + 1 < argc) {
            cl_path = argv[++i];
        }
    }

    const char *mode_str = (mode == MODE_GPU_FULL) ? "gpu_compute_full" : "interception_only";
    fprintf(stderr, "[ablation] mode=%s  n_target=%d\n", mode_str, n_target);

    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    g_cxl_base   = cxl_region_map(CXL_REGION_SIZE, NULL);
    g_cxl_size   = CXL_REGION_SIZE;
    g_is_standin = (strncmp(cxl_region_backing_path(), "/dev/dax", 8) != 0);
    fprintf(stderr, "[ablation] CXL backing=%s  standin=%d\n",
            cxl_region_backing_path(), g_is_standin);

    /* Load and attach BPF probes BEFORE initializing OpenCL.
     * This ensures the uprobe is registered in bpftime shm before the gNB starts.
     * OCL is initialized lazily once probes are live. */
    struct ldpc_probe_v5 *skel = ldpc_probe_v5__open();
    if (!skel) { fprintf(stderr, "[ablation] BPF open failed\n"); return 1; }
    if (ldpc_probe_v5__load(skel)) {
        fprintf(stderr, "[ablation] BPF load failed\n");
        ldpc_probe_v5__destroy(skel); return 1;
    }

    struct cxl_cfg_t cfg = {
        .region_base  = (uint64_t)(uintptr_t)g_cxl_base,
        .region_size  = (uint64_t)g_cxl_size,
        .out_base_off = (uint64_t)CXL_OUT_OFFSET,
        .is_standin   = g_is_standin ? 1u : 0u,
    };
    uint32_t k = 0;
    bpf_map_update_elem(bpf_map__fd(skel->maps.cxl_config), &k, &cfg, BPF_ANY);
    g_llr_stage_fd = bpf_map__fd(skel->maps.llr_staging);

    if (ldpc_probe_v5__attach(skel)) {
        fprintf(stderr, "[ablation] BPF attach failed\n");
        ldpc_probe_v5__destroy(skel); return 1;
    }
    fprintf(stderr, "[ablation] probes attached — collecting %d CBs...\n", n_target);

    /* Now init OCL (after probes are live so gNB can start in parallel) */
    if (mode == MODE_GPU_FULL) ocl_init(cl_path);

    memset(&g_rs, 0, sizeof(g_rs));
    g_rs.mode    = mode;
    g_rs.n_target = n_target;
    g_rs.samples_ns = calloc(MAX_SAMPLES, sizeof(uint64_t));
    if (!g_rs.samples_ns) { fprintf(stderr, "OOM\n"); return 1; }

    struct timespec ts0;
    clock_gettime(CLOCK_MONOTONIC, &ts0);
    g_rs.t_start_ns = (uint64_t)ts0.tv_sec * 1000000000ULL + ts0.tv_nsec;

    struct ring_buffer *rb = ring_buffer__new(
        bpf_map__fd(skel->maps.desc_ringbuf), handle_desc, NULL, NULL);
    if (!rb) { fprintf(stderr, "[ablation] ring_buffer__new failed\n"); return 1; }

    while (!g_stop && (int)g_rs.n_received < n_target) {
        ring_buffer__consume(rb);
        __asm__ volatile("pause" ::: "memory");
    }

    struct timespec ts_end;
    clock_gettime(CLOCK_MONOTONIC, &ts_end);
    uint64_t t_end_ns = (uint64_t)ts_end.tv_sec * 1000000000ULL + ts_end.tv_nsec;

    if (g_rs.cb_this_slot > g_rs.cb_per_slot_max)
        g_rs.cb_per_slot_max = g_rs.cb_this_slot;
    /* Prefer explicit override (--c-actual); fall back to inferred; then to v4 known-good=2.
     * The inter-gap heuristic misfires with bpftime burst delivery; override is safer. */
    int c_actual = (c_override > 0) ? c_override
                 : (g_rs.cb_per_slot_max > 0 && g_rs.cb_per_slot_max <= 24)
                   ? g_rs.cb_per_slot_max : 2;
    const char *c_source = (c_override > 0) ? "cli-override"
                         : (g_rs.cb_per_slot_max > 0 && g_rs.cb_per_slot_max <= 24)
                           ? "inferred" : "v4-fallback";

    int    ns  = g_rs.n_samples;
    double mn  = mean_us_fn(g_rs.samples_ns, ns);
    double p50 = pct_us(g_rs.samples_ns, ns, 50.0);
    double p95 = pct_us(g_rs.samples_ns, ns, 95.0);
    double p99 = pct_us(g_rs.samples_ns, ns, 99.0);

    double startup_s   = (g_rs.t_first_desc_ns > g_rs.t_start_ns)
                         ? (double)(g_rs.t_first_desc_ns - g_rs.t_start_ns) / 1e9 : 0.0;
    double active_s    = (g_rs.n_received > 1 && g_rs.t_last_desc_ns > g_rs.t_first_desc_ns)
                         ? (double)(g_rs.t_last_desc_ns - g_rs.t_first_desc_ns) / 1e9 : 1.0;
    double total_s     = (double)(t_end_ns - g_rs.t_start_ns) / 1e9;
    double active_rate = (active_s > 0) ? (double)g_rs.n_received / active_s : 0.0;
    double avg_rate    = (total_s  > 0) ? (double)g_rs.n_received / total_s  : 0.0;

    double mean_slot_us     = mn * c_actual;
    double proj_c24_slot_us = mean_slot_us * (24.0 / c_actual);
    double bit_density      = (g_rs.n_ocl_bits > 0)
                              ? (double)g_rs.n_ocl_ones / g_rs.n_ocl_bits : -1.0;

    fprintf(stderr, "\n[ablation] === %s REPORT ===\n", mode_str);
    fprintf(stderr, "n_received:         %lu\n",   (unsigned long)g_rs.n_received);
    fprintf(stderr, "n_samples:          %d\n",    ns);
    fprintf(stderr, "startup_s:          %.2f  (consumer-start → first CB)\n", startup_s);
    fprintf(stderr, "active_rate:        %.1f desc/s  (excludes startup)\n",   active_rate);
    fprintf(stderr, "avg_rate:           %.1f desc/s  (incl. startup)\n",      avg_rate);
    fprintf(stderr, "C_actual:           %d  (%s)\n", c_actual, c_source);
    fprintf(stderr, "mean_total_us:      %.3f\n", mn);
    fprintf(stderr, "p50_total_us:       %.3f\n", p50);
    fprintf(stderr, "p95_total_us:       %.3f\n", p95);
    fprintf(stderr, "p99_total_us:       %.3f\n", p99);
    fprintf(stderr, "mean_slot_us:       %.2f  (= mean_cb * C=%d)\n", mean_slot_us, c_actual);
    fprintf(stderr, "proj_c24_slot_us:   %.1f  (= mean_slot * 24/%d; DEV-009)\n",
            proj_c24_slot_us, c_actual);
    if (mode == MODE_GPU_FULL)
        fprintf(stderr, "bit_density:        %.4f  (OCL decoded ones/bits; >0.1 = non-trivial)\n",
                bit_density);

    /* CSV output */
    const char *csv_path = "../paper/results/latency_ladder_v2.csv";
    if (system("mkdir -p ../paper/results") < 0) { /* ignore */ }
    int write_header = 0;
    { FILE *f = fopen(csv_path, "r"); if (!f) write_header = 1; else fclose(f); }
    FILE *csv = fopen(csv_path, "a");
    if (csv) {
        if (write_header)
            fprintf(csv,
                "row,mean_us,p50_us,p95_us,p99_us,C_actual,N_CB,"
                "mean_slot_us,proj_c24_slot_us,notes\n"
                "baseline,%.1f,,,,%.0f,N/A,%.1f,%.1f,"
                "FIXED PRIMARY_CONFIG anchor (do not re-measure)\n",
                PRIMARY_CONFIG_US, (double)PRIMARY_CONFIG_C,
                PRIMARY_CONFIG_US, PRIMARY_CONFIG_US);

        fprintf(csv,
                "%s,%.3f,%.3f,%.3f,%.3f,%d,%lu,%.2f,%.1f,"
                "backing=%s standin=%d bit_density=%.4f "
                "active_rate=%.1f startup_s=%.2f\n",
                mode_str, mn, p50, p95, p99,
                c_actual, (unsigned long)g_rs.n_received,
                mean_slot_us, proj_c24_slot_us,
                cxl_region_backing_path(), g_is_standin,
                bit_density, active_rate, startup_s);
        fclose(csv);
        fprintf(stderr, "[ablation] CSV row written: %s\n", csv_path);
    }

    free(g_rs.samples_ns);
    ring_buffer__free(rb);
    ldpc_probe_v5__destroy(skel);
    return ((int)g_rs.n_received >= n_target) ? 0 : 1;
}
