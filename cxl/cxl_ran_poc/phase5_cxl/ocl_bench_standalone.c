/* ocl_bench_standalone.c — Standalone OCL LDPC decode benchmark.
 *
 * Reads LLR data from CXL stand-in region (pre-filled by interception_only run),
 * decodes N CBs with OpenCL, measures per-CB wall-clock time.
 *
 * Used for gpu_compute_full ablation row when bpftime agent UE segfault prevents
 * end-to-end run (DEV-021: gpu_compute_full standalone OCL timing).
 *
 * BG=1, Z=224 (matching Gate 3 / v4 observed config).
 *
 * Usage:
 *   ./ocl_bench_standalone [--n-cbs N] [--cl-path PATH] [--bg 1|2] [--z 224]
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <CL/cl.h>
#include "../gpu_daemon/ldpc_cl/bg_tables.h"
#include "cxl_region.h"

#define N_VN_INFO_BG1  22
#define N_VN_INFO_BG2  10
#define N_VN_FULL_BG1  68
#define N_VN_FULL_BG2  52
#define N_CN_BG1       46
#define N_CN_BG2       42
#define N_ITER         6
#define MAX_SAMPLES    16384
#define CXL_OUT_OFFSET (128UL * 1024 * 1024)

static cl_context       ocl_ctx;
static cl_device_id     ocl_dev;
static cl_program       ocl_prog;
static cl_kernel        ocl_kern;
static cl_command_queue ocl_q;

static void die(cl_int e, const char *w) {
    fprintf(stderr, "[ocl_bench] OCL error %d at %s\n", e, w); exit(1);
}
static char *read_file(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) { fprintf(stderr, "Cannot open %s\n", p); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    size_t nr = fread(buf, 1, sz, f); buf[nr] = '\0'; fclose(f); return buf;
}
static void ocl_init(const char *cl_path) {
    cl_int err;
    cl_uint np = 0; clGetPlatformIDs(0, NULL, &np);
    cl_platform_id *plats = malloc(np * sizeof(*plats));
    clGetPlatformIDs(np, plats, NULL);
    cl_uint nd = 0; clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, 0, NULL, &nd);
    cl_device_id *devs = malloc(nd * sizeof(*devs));
    clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, nd, devs, NULL);
    ocl_dev = devs[0]; free(devs); free(plats);
    char dn[128] = ""; clGetDeviceInfo(ocl_dev, CL_DEVICE_NAME, sizeof(dn), dn, NULL);
    fprintf(stderr, "[ocl_bench] OCL device: %s\n", dn);
    ocl_ctx = clCreateContext(NULL, 1, &ocl_dev, NULL, NULL, &err); if (err) die(err, "ctx");
    ocl_q   = clCreateCommandQueue(ocl_ctx, ocl_dev, 0, &err); if (err) die(err, "queue");
    char *src = read_file(cl_path);
    ocl_prog = clCreateProgramWithSource(ocl_ctx, 1, (const char **)&src, NULL, &err); free(src);
    if (err) die(err, "prog");
    err = clBuildProgram(ocl_prog, 1, &ocl_dev, "-D NO_EDGE=0xffff -D MAX_LS=384", NULL, NULL);
    if (err) {
        size_t ls = 0; clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &ls);
        char *log = malloc(ls + 1); clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, ls, log, NULL);
        log[ls] = '\0'; fprintf(stderr, "Build log:\n%s\n", log); free(log); exit(1);
    }
    ocl_kern = clCreateKernel(ocl_prog, "ldpc_decode", &err); if (err) die(err, "kernel");
}

static double run_one(int bg, int z, void *cxl_base, size_t llr_off, size_t out_off) {
    cl_int err;
    int n_vn_full = (bg == 1) ? N_VN_FULL_BG1 : N_VN_FULL_BG2;
    int n_cn      = (bg == 1) ? N_CN_BG1      : N_CN_BG2;
    int n_vn_info = (bg == 1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;
    int ls_idx    = (z <= 384) ? (int)(unsigned char)LS_TO_IDX[z] : 0;
    if (ls_idx == 255) ls_idx = 0;
    const uint16_t *shifts = (bg == 1)
        ? (const uint16_t *)BG1_SHIFTS[ls_idx]
        : (const uint16_t *)BG2_SHIFTS[ls_idx];
    size_t llr_bytes   = (size_t)n_vn_full * z;
    size_t out_bytes   = (size_t)((n_vn_info * z + 7) / 8);
    size_t c2v_bytes   = (size_t)n_cn * n_vn_full * z;
    size_t shift_bytes = (size_t)n_vn_full * n_cn * sizeof(uint16_t);

    cl_mem cl_llr    = clCreateBuffer(ocl_ctx, CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
                                      llr_bytes, (char *)cxl_base + llr_off, &err);
    cl_mem cl_out    = clCreateBuffer(ocl_ctx, CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
                                      out_bytes, (char *)cxl_base + out_off, &err);
    cl_mem cl_shifts = clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                      shift_bytes, (void *)shifts, &err);
    cl_mem cl_c2v    = clCreateBuffer(ocl_ctx, CL_MEM_READ_WRITE, c2v_bytes, NULL, &err);
    static const uint8_t zero = 0;
    clEnqueueFillBuffer(ocl_q, cl_c2v, &zero, 1, 0, c2v_bytes, 0, NULL, NULL);

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

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t gws = (size_t)n_cn;
    clEnqueueNDRangeKernel(ocl_q, ocl_kern, 1, NULL, &gws, NULL, 0, NULL, NULL);
    clFinish(ocl_q);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out);
    clReleaseMemObject(cl_shifts); clReleaseMemObject(cl_c2v);

    uint64_t ns = ((uint64_t)t1.tv_sec - t0.tv_sec) * 1000000000ULL
                + (uint64_t)t1.tv_nsec - t0.tv_nsec;
    return (double)ns / 1000.0;  /* return us */
}

static int cmp_dbl(const void *a, const void *b) {
    double x = *(double *)a, y = *(double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

int main(int argc, char **argv) {
    int         n_cbs   = 1000;
    int         bg      = 1;
    int         z       = 224;
    int         c_actual = 2;
    const char *cl_path = "../gpu_daemon/ldpc_cl/ldpc_decode.cl";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--n-cbs")   && i+1 < argc) n_cbs    = atoi(argv[++i]);
        if (!strcmp(argv[i], "--bg")      && i+1 < argc) bg       = atoi(argv[++i]);
        if (!strcmp(argv[i], "--z")       && i+1 < argc) z        = atoi(argv[++i]);
        if (!strcmp(argv[i], "--c-actual")&& i+1 < argc) c_actual = atoi(argv[++i]);
        if (!strcmp(argv[i], "--cl-path") && i+1 < argc) cl_path  = argv[++i];
    }

    void *cxl_base = cxl_region_map(CXL_REGION_SIZE, NULL);
    fprintf(stderr, "[ocl_bench] backing=%s  BG=%d Z=%d n_cbs=%d\n",
            cxl_region_backing_path(), bg, z, n_cbs);

    ocl_init(cl_path);
    if (n_cbs > MAX_SAMPLES) n_cbs = MAX_SAMPLES;

    double *samples = malloc(n_cbs * sizeof(double));

    /* LLR slots: matches ablation.c's slot layout (slot × LLR_SLOT_STRIDE) */
    size_t llr_slot_stride = (size_t)(N_VN_FULL_BG1 * 384 + 63) / 64 * 64;  /* 26176 */
    size_t out_slot_stride = 16384;
    size_t n_llr_slots     = 16;   /* matches BPF program N_LLR_SLOTS=16 */

    /* Warm up: run one decode to JIT-compile any lazy OCL steps */
    run_one(bg, z, cxl_base, 0, (size_t)CXL_OUT_OFFSET);

    for (int i = 0; i < n_cbs; i++) {
        size_t slot    = (size_t)i % n_llr_slots;
        size_t llr_off = slot * llr_slot_stride;
        size_t out_off = CXL_OUT_OFFSET + slot * out_slot_stride;
        samples[i]     = run_one(bg, z, cxl_base, llr_off, out_off);
    }

    qsort(samples, n_cbs, sizeof(double), cmp_dbl);
    double mn = 0;
    for (int i = 0; i < n_cbs; i++) mn += samples[i];
    mn /= n_cbs;
    double p50 = samples[(int)(0.50 * n_cbs)];
    double p95 = samples[(int)(0.95 * n_cbs)];
    double p99 = samples[(int)(0.99 * n_cbs)];

    /* Combine with interception_only overhead (DEV-021) */
    double intercept_p50_us = 1075.916;   /* from Gate 4 interception_only run */
    double total_mn_us  = mn   + intercept_p50_us;
    double total_p50_us = p50  + intercept_p50_us;

    double mean_slot_us     = total_mn_us * c_actual;
    double proj_c24_slot_us = mean_slot_us * (24.0 / c_actual);

    fprintf(stderr, "\n[ocl_bench] === gpu_compute_full STANDALONE OCL REPORT ===\n");
    fprintf(stderr, "n_cbs:              %d\n",  n_cbs);
    fprintf(stderr, "ocl_only_mean_us:   %.1f\n", mn);
    fprintf(stderr, "ocl_only_p50_us:    %.1f\n", p50);
    fprintf(stderr, "ocl_only_p95_us:    %.1f\n", p95);
    fprintf(stderr, "ocl_only_p99_us:    %.1f\n", p99);
    fprintf(stderr, "intercept_p50_us:   %.3f  (from interception_only Gate 4)\n", intercept_p50_us);
    fprintf(stderr, "total_p50_us:       %.1f  (OCL + interception overhead)\n", total_p50_us);
    fprintf(stderr, "mean_slot_us:       %.1f  (= total_mean * C=%d)\n", mean_slot_us, c_actual);
    fprintf(stderr, "proj_c24_slot_us:   %.1f  (= mean_slot * 24/%d; DEV-009)\n",
            proj_c24_slot_us, c_actual);
    fprintf(stderr, "note: DEV-021 — standalone OCL timing (bpftime UE segfault prevents E2E)\n");

    /* Append CSV row */
    if (system("mkdir -p ../paper/results") < 0) { /* ignore */ }
    FILE *csv = fopen("../paper/results/latency_ladder_v2_v5.csv", "a");
    if (csv) {
        fprintf(csv,
                "gpu_compute_full,%.3f,%.3f,%.3f,%.3f,%d,%d,%.1f,%.1f,"
                "standalone-OCL backing=%s ocl_only_mean_us=%.1f "
                "intercept_overhead_us=%.3f DEV-021\n",
                total_mn_us, total_p50_us, p95 + intercept_p50_us, p99 + intercept_p50_us,
                c_actual, n_cbs,
                mean_slot_us, proj_c24_slot_us,
                cxl_region_backing_path(), mn, intercept_p50_us);
        fclose(csv);
        fprintf(stderr, "[ocl_bench] CSV row written\n");
    }

    free(samples);
    return 0;
}
