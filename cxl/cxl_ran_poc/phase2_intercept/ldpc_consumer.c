/* ldpc_consumer.c — Phase 2 Gate 2 loader+consumer.
 *
 * Loads ldpc_probe.bpf into bpftime's syscall-server (run under
 * LD_PRELOAD=libbpftime-syscall-server.so), polls for new CB descriptors,
 * and for each one: reads the LLR data from the llr_copy BPF map and runs
 * our OpenCL LDPC decoder kernel on it.  On SIGINT, prints the summary.
 *
 * Usage:
 *   LD_PRELOAD=.../libbpftime-syscall-server.so SPDLOG_LEVEL=warn \
 *     BPFTIME_VM_NAME=ubpf ./ldpc_consumer <N_target_cbs>
 *
 * Then start the OAI gNB (in gnb-ns) with:
 *   LD_PRELOAD=.../libbpftime-agent.so ip netns exec gnb-ns ... nr-softmodem ...
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "ldpc_probe.skel.h"

/* libbpf >= 0.6: bpf_num_possible_cpus() */
#ifndef BPF_MAX_CPUS
#define BPF_MAX_CPUS 512
#endif

/* OpenCL */
#include <CL/cl.h>

/* bg_tables.h from Phase 1 */
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
#define POLL_MS        50   /* poll interval ms */

/* ---- globals ---- */
static volatile int g_stop = 0;
static void on_sig(int s) { (void)s; g_stop = 1; }

/* ---- OpenCL state ---- */
static cl_context   ocl_ctx;
static cl_device_id ocl_dev;
static cl_program   ocl_prog;
static cl_kernel    ocl_kern;
static cl_command_queue ocl_q;

static void ocl_die(cl_int err, const char *w) {
    fprintf(stderr, "OpenCL error %d at %s\n", err, w);
    exit(1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); rewind(f);
    char *buf = malloc(sz + 1);
    if (fread(buf, 1, sz, f) != (size_t)sz) { fprintf(stderr, "short read %s\n", path); exit(1); }
    fclose(f);
    buf[sz] = '\0';
    return buf;
}

static void ocl_init(const char *cl_path) {
    cl_int err;
    cl_uint nplat; clGetPlatformIDs(0, NULL, &nplat);
    cl_platform_id *plats = malloc(nplat * sizeof(*plats));
    clGetPlatformIDs(nplat, plats, NULL);
    cl_uint ndev; clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, 0, NULL, &ndev);
    cl_device_id *devs = malloc(ndev * sizeof(*devs));
    clGetDeviceIDs(plats[0], CL_DEVICE_TYPE_ALL, ndev, devs, NULL);
    ocl_dev = devs[0]; free(devs); free(plats);

    char dname[128] = "";
    clGetDeviceInfo(ocl_dev, CL_DEVICE_NAME, sizeof(dname), dname, NULL);
    fprintf(stderr, "[consumer] OpenCL device: %s\n", dname);

    ocl_ctx = clCreateContext(NULL, 1, &ocl_dev, NULL, NULL, &err);
    if (err) ocl_die(err, "clCreateContext");
    cl_queue_properties qprops[] = {0};
    ocl_q = clCreateCommandQueueWithProperties(ocl_ctx, ocl_dev, qprops, &err);
    if (err) ocl_die(err, "clCreateCommandQueue");

    char *src = read_file(cl_path);
    const char *srcs[1] = { src };
    ocl_prog = clCreateProgramWithSource(ocl_ctx, 1, srcs, NULL, &err);
    free(src);
    if (err) ocl_die(err, "clCreateProgramWithSource");

    char opts[128];
    snprintf(opts, sizeof(opts), "-D NO_EDGE=0xffff -D MAX_LS=384");
    err = clBuildProgram(ocl_prog, 1, &ocl_dev, opts, NULL, NULL);
    if (err) {
        size_t log_sz;
        clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, 0, NULL, &log_sz);
        char *log = malloc(log_sz + 1);
        clGetProgramBuildInfo(ocl_prog, ocl_dev, CL_PROGRAM_BUILD_LOG, log_sz, log, NULL);
        log[log_sz] = '\0';
        fprintf(stderr, "OpenCL build error:\n%s\n", log); free(log);
        exit(1);
    }
    ocl_kern = clCreateKernel(ocl_prog, "ldpc_decode", &err);
    if (err) ocl_die(err, "clCreateKernel");
}

/* Run OpenCL decoder on one CB and return number of bits that are '1'.
 * llr_in: int8_t[n_vn_full * Z], already in our kernel's format.
 * out_bits: uint8_t[ceil(n_vn_info*Z/8)], filled by kernel.
 */
static int ocl_decode_cb(const int8_t *llr_in, uint8_t *out_bits,
                          int bg, int Z)
{
    cl_int err;
    int n_vn_full = (bg == 1) ? N_VN_FULL_BG1 : N_VN_FULL_BG2;
    int n_cn      = (bg == 1) ? N_CN_BG1      : N_CN_BG2;
    int n_vn_info = (bg == 1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;
    int ls_idx = (Z >= 0 && Z <= 384) ? LS_TO_IDX[Z] : 0;
    if (ls_idx == 255) ls_idx = 0;  /* invalid Z — fallback, shouldn't happen */

    size_t llr_bytes  = (size_t)n_vn_full * Z;
    size_t out_bytes  = (size_t)((n_vn_info * Z + 7) / 8);
    size_t c2v_bytes  = (size_t)n_cn * n_vn_full * Z;
    size_t shift_cnt  = (size_t)n_cn * n_vn_full;

    const uint16_t *shifts = (bg == 1)
        ? (const uint16_t *)BG1_SHIFTS[ls_idx]
        : (const uint16_t *)BG2_SHIFTS[ls_idx];

    cl_mem cl_llr   = clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     llr_bytes, (void *)llr_in, &err);
    if (err) { fprintf(stderr, "cl_llr alloc err %d\n", err); return -1; }
    cl_mem cl_out   = clCreateBuffer(ocl_ctx, CL_MEM_WRITE_ONLY, out_bytes, NULL, &err);
    if (err) { clReleaseMemObject(cl_llr); return -1; }
    cl_mem cl_shifts= clCreateBuffer(ocl_ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                     shift_cnt * sizeof(uint16_t), (void *)shifts, &err);
    if (err) { clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out); return -1; }
    cl_mem cl_c2v   = clCreateBuffer(ocl_ctx, CL_MEM_READ_WRITE, c2v_bytes, NULL, &err);
    if (err) { clReleaseMemObject(cl_llr); clReleaseMemObject(cl_out); clReleaseMemObject(cl_shifts); return -1; }

    /* zero c2v */
    int8_t zero = 0;
    err = clEnqueueFillBuffer(ocl_q, cl_c2v, &zero, 1, 0, c2v_bytes, 0, NULL, NULL);
    if (err) err = 0; /* fall back: explicit zero below */
    {
        static int8_t *zeros = NULL;
        if (!zeros) { zeros = calloc(1, 68*384*46); }
        clEnqueueWriteBuffer(ocl_q, cl_c2v, CL_TRUE, 0, c2v_bytes, zeros, 0, NULL, NULL);
    }

    /* also zero output */
    int8_t zero8 = 0;
    clEnqueueFillBuffer(ocl_q, cl_out, &zero8, 1, 0, out_bytes, 0, NULL, NULL);

    int n_vn_full_a = n_vn_full, n_cn_a = n_cn, n_vn_info_a = n_vn_info;
    int ls_a = Z, n_iter_a = N_ITER, cb_off_a = 0;
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

    size_t gws = 1;
    err = clEnqueueNDRangeKernel(ocl_q, ocl_kern, 1, NULL, &gws, &gws, 0, NULL, NULL);
    if (err) { fprintf(stderr, "enqueue err %d\n", err); goto cleanup; }
    clFinish(ocl_q);

    err = clEnqueueReadBuffer(ocl_q, cl_out, CL_TRUE, 0, out_bytes, out_bits, 0, NULL, NULL);
    if (err) { fprintf(stderr, "read err %d\n", err); goto cleanup; }

cleanup:
    clReleaseMemObject(cl_llr);
    clReleaseMemObject(cl_out);
    clReleaseMemObject(cl_shifts);
    clReleaseMemObject(cl_c2v);

    /* count set bits in output */
    int ones = 0;
    for (size_t i = 0; i < out_bytes; i++) {
        uint8_t b = out_bits[i];
        while (b) { ones += b & 1; b >>= 1; }
    }
    return ones;
}

int main(int argc, char **argv)
{
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int n_target = (argc > 1) ? atoi(argv[1]) : 100;
    fprintf(stderr, "[consumer] target: %d CB decodes\n", n_target);

    /* ---- load OpenCL ---- */
    const char *cl_path = "../gpu_daemon/ldpc_cl/ldpc_decode.cl";
    ocl_init(cl_path);

    /* ---- load BPF ---- */
    libbpf_set_print(NULL);   /* suppress libbpf noise */
    struct ldpc_probe *skel = ldpc_probe__open();
    if (!skel) { fprintf(stderr, "BPF open failed\n"); return 1; }
    if (ldpc_probe__load(skel)) { fprintf(stderr, "BPF load failed\n"); return 1; }
    if (ldpc_probe__attach(skel)) { fprintf(stderr, "BPF attach failed\n"); return 1; }

    fprintf(stderr, "[consumer] probes attached, polling...\n");

    int mfd_slot_cnt  = bpf_map__fd(skel->maps.slot_counter);
    int mfd_cb_cnt    = bpf_map__fd(skel->maps.cb_counter);
    int mfd_cb_desc   = bpf_map__fd(skel->maps.cb_desc);
    int mfd_llr       = bpf_map__fd(skel->maps.llr_copy);

    __u32 k = 0;
    uint64_t last_ts = 0;
    int  n_decoded = 0;
    uint8_t out_bits[68 * 384 / 8 + 16];

    /* struct mirroring BPF cb_desc_t */
    struct {
        uint64_t timestamp_ns;
        uint16_t Z;
        uint8_t  BG;
        uint8_t  pad[5];
    } cb_d;

    int8_t llr_buf[LLR_BUF_BYTES];

    while (!g_stop && n_decoded < n_target) {
        /* check for new CB */
        if (bpf_map_lookup_elem(mfd_cb_desc, &k, &cb_d) == 0) {
            if (cb_d.timestamp_ns != last_ts && cb_d.BG >= 1 && cb_d.Z >= 64) {
                last_ts = cb_d.timestamp_ns;

                /* read LLR from BPF map */
                if (bpf_map_lookup_elem(mfd_llr, &k, llr_buf) == 0) {
                    memset(out_bits, 0, sizeof(out_bits));
                    int ones = ocl_decode_cb(llr_buf, out_bits, cb_d.BG, cb_d.Z);
                    n_decoded++;

                    int n_info = (cb_d.BG == 1) ? N_VN_INFO_BG1 : N_VN_INFO_BG2;
                    int out_bytes = (n_info * cb_d.Z + 7) / 8;

                    /* count non-zero input bytes (proves non-trivial channel data) */
                    int nz = 0;
                    int total = (cb_d.BG == 1) ? (68 * cb_d.Z) : (52 * cb_d.Z);
                    for (int i = 0; i < total; i++) nz += (llr_buf[i] != 0) ? 1 : 0;

                    fprintf(stdout, "CB %4d: BG%d Z=%3d  llr_nonzero=%d/%d  "
                            "decoded_ones=%d/%d\n",
                            n_decoded, cb_d.BG, cb_d.Z,
                            nz, total, ones, out_bytes * 8);
                    fflush(stdout);
                }
            }
        }

        struct timespec ts = {0, POLL_MS * 1000000LL};
        nanosleep(&ts, NULL);
    }

    /* final summary — aggregate PERCPU_ARRAY counters across all CPUs */
    int n_cpus = libbpf_num_possible_cpus();
    if (n_cpus <= 0 || n_cpus > BPF_MAX_CPUS) n_cpus = 1;

    uint64_t cb_percpu[BPF_MAX_CPUS]  = {0};
    uint64_t sl_percpu[BPF_MAX_CPUS]  = {0};
    bpf_map_lookup_elem(mfd_cb_cnt,   &k, cb_percpu);
    bpf_map_lookup_elem(mfd_slot_cnt, &k, sl_percpu);

    uint64_t cb_cnt = 0, sl_cnt = 0;
    for (int i = 0; i < n_cpus; i++) { cb_cnt += cb_percpu[i]; sl_cnt += sl_percpu[i]; }

    fprintf(stdout,
            "\n[consumer] SUMMARY:\n"
            "  slot_calls (nrLDPC_coding_decoder):  %llu\n"
            "  cb_calls   (LDPCdecoder):             %llu\n"
            "  cbs_decoded_by_opencl:                %d\n"
            "  n_cpus_aggregated:                    %d\n"
            "  cb_per_slot_ratio:                    %.3f\n",
            (unsigned long long)sl_cnt,
            (unsigned long long)cb_cnt,
            n_decoded,
            n_cpus,
            sl_cnt > 0 ? (double)cb_cnt / sl_cnt : 0.0);
    fflush(stdout);

    ldpc_probe__destroy(skel);
    clReleaseKernel(ocl_kern);
    clReleaseProgram(ocl_prog);
    clReleaseCommandQueue(ocl_q);
    clReleaseContext(ocl_ctx);
    return 0;
}
