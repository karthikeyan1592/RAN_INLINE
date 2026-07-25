/* ldpc_measure.c — Phase 5 ablation timing harness.
 *
 * Runs two back-to-back measurement passes over N_SLOTS slots:
 *   pass 0 (interception_only): probes fire, LLR captured, NO OpenCL.
 *   pass 1 (gpu_compute_full):  probes fire, LLR captured, full OpenCL decode.
 *
 * For each CB event, records:
 *   - t_probe_ns  : timestamp from the BPF probe (bpf_ktime_get_ns inside LDPCdecoder)
 *   - t_consumer_ns: time consumer finishes processing this CB
 *   => per-CB overhead = t_consumer_ns - t_probe_ns
 *   => per-slot overhead = sum over C_actual CBs in that slot
 *
 * Outputs:
 *   paper/results/ablation_raw.csv  (all CB samples, both passes)
 *   paper/results/latency_ladder_v2.csv  (aggregate per-pass stats)
 *
 * Usage:
 *   LD_PRELOAD=.../libbpftime-syscall-server.so SPDLOG_LEVEL=warn \
 *     BPFTIME_VM_NAME=ubpf ./ldpc_measure <N_slots_target>
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <unistd.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ldpc_probe.skel.h"

#include <CL/cl.h>
#include "../gpu_daemon/ldpc_cl/bg_tables.h"

/* ---- constants ---- */
#define LLR_BUF_BYTES  (68 * 384)
#define N_VN_INFO_BG1  22
#define N_VN_INFO_BG2  10
#define N_VN_FULL_BG1  68
#define N_VN_FULL_BG2  52
#define N_CN_BG1       46
#define N_CN_BG2       42
#define N_ITER          6
#define BPF_MAX_CPUS   512
#define POLL_NS        2000000LL   /* 2 ms poll */
#define MAX_SAMPLES   200000

/* ---- OpenCL globals ---- */
static cl_context   ocl_ctx;
static cl_device_id ocl_dev;
static cl_program   ocl_prog;
static cl_kernel    ocl_kern;
static cl_command_queue ocl_q;

static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

static uint64_t mono_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static char *read_file(const char *p) {
    FILE *f = fopen(p, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *b = malloc(sz + 1);
    if (fread(b, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read\n"); exit(1); }
    fclose(f); b[sz] = '\0'; return b;
}

static void ocl_init(void) {
    cl_int err;
    cl_uint nplat; clGetPlatformIDs(0, NULL, &nplat);
    cl_platform_id *plats = malloc(nplat * sizeof(*plats));
    clGetPlatformIDs(nplat, plats, NULL);
    cl_uint ndev; clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, 0, NULL, &ndev);
    cl_device_id *devs = malloc(ndev * sizeof(*devs));
    clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, ndev, devs, NULL);
    ocl_dev = devs[0]; free(devs); free(plats);

    ocl_ctx = clCreateContext(NULL, 1, &ocl_dev, NULL, NULL, &err);
    cl_queue_properties qp[] = {0};
    ocl_q = clCreateCommandQueueWithProperties(ocl_ctx, ocl_dev, qp, &err);

    const char *src = read_file("../gpu_daemon/ldpc_cl/ldpc_decode.cl");
    const char *srcs[1] = {src};
    ocl_prog = clCreateProgramWithSource(ocl_ctx, 1, srcs, NULL, &err);
    free((void*)src);
    err = clBuildProgram(ocl_prog, 1, &ocl_dev, "-D NO_EDGE=0xffff -D MAX_LS=384", NULL, NULL);
    if (err) { fprintf(stderr, "OCL build failed %d\n", err); exit(1); }
    ocl_kern = clCreateKernel(ocl_prog, "ldpc_decode", &err);
    if (err) { fprintf(stderr, "OCL kernel failed %d\n", err); exit(1); }
    fprintf(stderr, "[measure] OpenCL ready\n");
}

/* Returns wall-clock ns spent in OpenCL decode. 0 if compute=0. */
static uint64_t run_ocl(const int8_t *llr_in, int bg, int Z) {
    cl_int err;
    int n_vn_full = (bg==1) ? N_VN_FULL_BG1 : N_VN_FULL_BG2;
    int n_cn      = (bg==1) ? N_CN_BG1 : N_CN_BG2;
    int n_vn_info = (bg==1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;
    int ls_idx    = (Z>=0 && Z<=384) ? LS_TO_IDX[Z] : 0;
    if (ls_idx == 255) ls_idx = 0;

    size_t llr_bytes = (size_t)n_vn_full * Z;
    size_t out_bytes = (size_t)((n_vn_info*Z+7)/8);
    size_t c2v_bytes = (size_t)n_cn * n_vn_full * Z;
    const uint16_t *shifts = (bg==1) ? (const uint16_t*)BG1_SHIFTS[ls_idx]
                                      : (const uint16_t*)BG2_SHIFTS[ls_idx];
    static int8_t zeros[68*384*46];  /* static — zero-initialised */

    cl_mem cl_llr  = clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, llr_bytes, (void*)llr_in, &err);
    cl_mem cl_out  = clCreateBuffer(ocl_ctx, CL_MEM_WRITE_ONLY, out_bytes, NULL, &err);
    cl_mem cl_sh   = clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR, (size_t)n_cn*n_vn_full*sizeof(uint16_t), (void*)shifts, &err);
    cl_mem cl_c2v  = clCreateBuffer(ocl_ctx, CL_MEM_READ_WRITE, c2v_bytes, NULL, &err);
    clEnqueueWriteBuffer(ocl_q, cl_c2v, CL_TRUE, 0, c2v_bytes, zeros, 0, NULL, NULL);

    int vf=n_vn_full, cn=n_cn, vi=n_vn_info, ls=Z, ni=N_ITER, co=0;
    clSetKernelArg(ocl_kern,0,sizeof(cl_mem),&cl_llr);
    clSetKernelArg(ocl_kern,1,sizeof(cl_mem),&cl_out);
    clSetKernelArg(ocl_kern,2,sizeof(cl_mem),&cl_sh);
    clSetKernelArg(ocl_kern,3,sizeof(cl_mem),&cl_c2v);
    clSetKernelArg(ocl_kern,4,sizeof(cl_int),&vf);
    clSetKernelArg(ocl_kern,5,sizeof(cl_int),&cn);
    clSetKernelArg(ocl_kern,6,sizeof(cl_int),&vi);
    clSetKernelArg(ocl_kern,7,sizeof(cl_int),&ls);
    clSetKernelArg(ocl_kern,8,sizeof(cl_int),&ni);
    clSetKernelArg(ocl_kern,9,sizeof(cl_int),&co);

    size_t gws=1;
    uint64_t t0 = mono_ns();
    clEnqueueNDRangeKernel(ocl_q, ocl_kern, 1, NULL, &gws, &gws, 0, NULL, NULL);
    clFinish(ocl_q);
    uint64_t elapsed = mono_ns() - t0;

    uint8_t *out = malloc(out_bytes);
    clEnqueueReadBuffer(ocl_q, cl_out, CL_TRUE, 0, out_bytes, out, 0, NULL, NULL);
    free(out);
    clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out);
    clReleaseMemObject(cl_sh);  clReleaseMemObject(cl_c2v);
    return elapsed;
}

/* ---- per-sample record ---- */
struct sample {
    uint64_t probe_ts_ns;    /* bpf_ktime_get_ns() from inside LDPCdecoder */
    uint64_t consumer_ts_ns; /* mono_ns() when consumer finishes this CB */
    uint64_t ocl_ns;         /* 0 for interception_only pass */
    uint8_t  bg;
    uint16_t Z;
    uint8_t  pass;           /* 0=interception_only, 1=gpu_compute_full */
};

static struct sample samples[MAX_SAMPLES];
static int n_samples = 0;

/* prime_ts: skip any cb_d.ts <= prime_ts at loop start (for pass transitions) */
static int run_pass(struct ldpc_probe *skel, int compute, int n_slots_target,
                    uint64_t prime_ts)
{
    int mfd_cb_desc = bpf_map__fd(skel->maps.cb_desc);
    int mfd_llr     = bpf_map__fd(skel->maps.llr_copy);

    struct { uint64_t ts; uint16_t Z; uint8_t BG; uint8_t pad[5]; } cb_d;
    int8_t llr_buf[LLR_BUF_BYTES];
    __u32 k = 0;
    uint64_t last_ts = prime_ts;  /* skip stale entry if primed */
    int n_cb = 0;
    int n_slots_seen = 0;
    uint64_t last_event_ns = mono_ns();
    const uint64_t TIMEOUT_NS = 45ULL * 1000000000ULL;  /* 45s no-event timeout */

    fprintf(stderr, "[measure] pass %d (%s): target %d slots (prime_ts=%llu)\n",
            compute, compute ? "gpu_compute_full" : "interception_only",
            n_slots_target, (unsigned long long)prime_ts);

    while (!g_stop && n_slots_seen < n_slots_target) {
        if (bpf_map_lookup_elem(mfd_cb_desc, &k, &cb_d) == 0) {
            if (cb_d.ts != last_ts && cb_d.BG >= 1 && cb_d.Z >= 64) {
                last_ts = cb_d.ts;
                last_event_ns = mono_ns();
                bpf_map_lookup_elem(mfd_llr, &k, llr_buf);

                uint64_t ocl_ns = 0;
                if (compute) ocl_ns = run_ocl(llr_buf, cb_d.BG, cb_d.Z);
                uint64_t t_done = mono_ns();

                if (n_samples < MAX_SAMPLES) {
                    samples[n_samples].probe_ts_ns    = cb_d.ts;
                    samples[n_samples].consumer_ts_ns = t_done;
                    samples[n_samples].ocl_ns         = ocl_ns;
                    samples[n_samples].bg             = cb_d.BG;
                    samples[n_samples].Z              = cb_d.Z;
                    samples[n_samples].pass           = (uint8_t)compute;
                    n_samples++;
                }
                n_cb++;
            }
        }

        /* timeout: abort if no new CB events for TIMEOUT_NS */
        if (mono_ns() - last_event_ns > TIMEOUT_NS) {
            fprintf(stderr, "[measure] TIMEOUT: no new CBs for 15s — gNB may have stopped\n");
            break;
        }

        /* derive slot count from CB count: C_actual=2 CBs/slot (DEV-009) */
        n_slots_seen = n_cb / 2;

        struct timespec ts = {0, POLL_NS};
        nanosleep(&ts, NULL);
    }

    fprintf(stderr, "[measure] pass %d done: %d CB samples, %d slots seen\n",
            compute, n_cb, n_slots_seen);
    return n_slots_seen;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(uint64_t*)a, y = *(uint64_t*)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
    int n_slots = (argc > 1) ? atoi(argv[1]) : 1000;

    ocl_init();

    libbpf_set_print(NULL);
    struct ldpc_probe *skel = ldpc_probe__open();
    if (!skel || ldpc_probe__load(skel) || ldpc_probe__attach(skel)) {
        fprintf(stderr, "BPF init failed\n"); return 1;
    }
    fprintf(stderr, "[measure] probes attached\n");

    /* pass 0: interception_only (no compute) */
    run_pass(skel, 0, n_slots, /*prime_ts=*/0);

    /* brief pause, then prime pass 1 so stale cb_desc entry is skipped */
    sleep(2);
    g_stop = 0;
    {
        struct { uint64_t ts; uint16_t Z; uint8_t BG; uint8_t pad[5]; } prime_d;
        __u32 k0 = 0;
        uint64_t prime_ts = 0;
        if (bpf_map_lookup_elem(bpf_map__fd(skel->maps.cb_desc), &k0, &prime_d) == 0)
            prime_ts = prime_d.ts;
        fprintf(stderr, "[measure] priming pass 1 with ts=%llu\n",
                (unsigned long long)prime_ts);
        /* pass 1: gpu_compute_full */
        run_pass(skel, 1, n_slots, prime_ts);
    }

    ldpc_probe__destroy(skel);
    clReleaseKernel(ocl_kern); clReleaseProgram(ocl_prog);
    clReleaseCommandQueue(ocl_q); clReleaseContext(ocl_ctx);

    /* ---- write ablation_raw.csv ---- */
    const char *raw_path = "../paper/results/ablation_raw.csv";
    FILE *raw = fopen(raw_path, "w");
    if (!raw) { perror("ablation_raw.csv"); return 1; }
    fprintf(raw, "pass,bg,Z,probe_ts_ns,consumer_ts_ns,overhead_ns,ocl_ns\n");
    for (int i = 0; i < n_samples; i++) {
        uint64_t oh = (samples[i].consumer_ts_ns > samples[i].probe_ts_ns)
                      ? samples[i].consumer_ts_ns - samples[i].probe_ts_ns : 0;
        fprintf(raw, "%d,%d,%d,%llu,%llu,%llu,%llu\n",
                samples[i].pass, samples[i].bg, samples[i].Z,
                (unsigned long long)samples[i].probe_ts_ns,
                (unsigned long long)samples[i].consumer_ts_ns,
                (unsigned long long)oh,
                (unsigned long long)samples[i].ocl_ns);
    }
    fclose(raw);
    fprintf(stderr, "[measure] wrote %s (%d rows)\n", raw_path, n_samples);

    /* ---- compute per-pass stats (overhead_ns per CB) ---- */
    /* C_actual=2 CB/slot (DEV-009), so per-slot = 2 × per-CB */
    for (int pass = 0; pass <= 1; pass++) {
        uint64_t vals[MAX_SAMPLES];
        int cnt = 0;
        for (int i = 0; i < n_samples; i++) {
            if (samples[i].pass != pass) continue;
            uint64_t oh = (samples[i].consumer_ts_ns > samples[i].probe_ts_ns)
                          ? samples[i].consumer_ts_ns - samples[i].probe_ts_ns : 0;
            /* for gpu_compute_full, overhead includes ocl_ns */
            vals[cnt++] = (pass == 1) ? samples[i].ocl_ns : oh;
        }
        if (cnt == 0) continue;
        qsort(vals, cnt, sizeof(uint64_t), cmp_u64);
        double sum = 0; for (int i = 0; i < cnt; i++) sum += vals[i];
        double mean = sum / cnt;
        double sq = 0; for (int i = 0; i < cnt; i++) {
            double d = vals[i] - mean; sq += d*d;
        }
        double stddev = (cnt > 1) ? sqrt(sq/(cnt-1)) : 0;
        uint64_t p50 = vals[cnt/2];
        uint64_t p95 = vals[(int)(cnt*0.95)];
        uint64_t p99 = vals[(int)(cnt*0.99)];
        const char *label = (pass == 0) ? "interception_only" : "gpu_compute_full";
        const char *metric = (pass == 0) ? "per_cb_overhead_ns" : "ocl_per_cb_ns";
        fprintf(stdout, "\n[measure] pass %d (%s) — %s — n=%d\n",
                pass, label, metric, cnt);
        fprintf(stdout, "  mean=%.0f  p50=%llu  p95=%llu  p99=%llu  stddev=%.0f\n",
                mean, (unsigned long long)p50,
                (unsigned long long)p95, (unsigned long long)p99, stddev);
        /* per-slot = mean × C_actual=2 */
        fprintf(stdout, "  per_slot_us (×C_actual=2): %.3f\n", mean * 2.0 / 1000.0);
    }

    return 0;
}
