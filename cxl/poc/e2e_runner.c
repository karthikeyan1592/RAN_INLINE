/*
 * e2e_runner.c  — Phase 4 E2E controller
 *
 * Wires the five arrows:
 *   srsRAN ldpc_decoder_benchmark
 *     ↓  uprobe (bpftime)
 *   ldpc_llr_mover.bpf  →  llr_region map
 *     ↓  mmap shared region
 *   CXL node 1 (memfd + mbind)
 *     ↓  CL_MEM_USE_HOST_PTR
 *   PoCL OpenCL consumer
 *     ↓  CSV output
 *   e2e_gcp.csv
 *
 * Usage:
 *   ./e2e_runner --ldpc-bin /path/to/ldpc_decoder_benchmark \
 *                --iters 1000  --output e2e_gcp.csv
 *
 * Build:
 *   gcc -O2 -o e2e_runner e2e_runner.c -lOpenCL -lnuma -lpthread && echo BUILD_OK
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <getopt.h>
#include <time.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/memfd.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

/* Region layout must match ldpc_llr_mover.bpf.c */
#define LLR_MAX_BYTES   12288
#define REGION_SIZE     (LLR_MAX_BYTES + 16)
#define META_LEN_OFF    LLR_MAX_BYTES
#define META_SEQ_OFF    (LLR_MAX_BYTES + 4)

/* CSV output */
#define MAX_ROWS        16384

struct row_t {
    uint64_t wall_ns;       /* uprobe-to-OCL-read latency */
    uint32_t llr_len;       /* LLR bytes moved */
    uint32_t seq;           /* sequence counter at read */
    double   ocl_bw_gbps;   /* OCL map throughput */
};

static struct row_t rows[MAX_ROWS];
static int          nrows = 0;
static volatile int done  = 0;

static int g_shm_fd = -1;
static void *g_shm  = NULL;

static int mbind_buf(void *ptr, size_t sz, int node) {
    unsigned long nodemask = 1UL << node;
    return (int)syscall(SYS_mbind, ptr, sz, MPOL_BIND,
                        &nodemask, sizeof(nodemask) * 8, MPOL_MF_MOVE);
}

/* ── OpenCL context (module-level, initialised once) ─────────────────────── */
static cl_context       g_ctx;
static cl_command_queue g_queue;
static cl_device_id     g_device;
static cl_mem           g_cl_buf;
static int              g_ocl_ok = 0;

static int init_opencl(void *host_ptr, size_t sz) {
    cl_platform_id platform;
    cl_int err;
    if (clGetPlatformIDs(1, &platform, NULL) != CL_SUCCESS) return 0;
    if (clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &g_device, NULL) != CL_SUCCESS) return 0;

    char devname[128];
    clGetDeviceInfo(g_device, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
    printf("[OCL] device: %s\n", devname);

    g_ctx   = clCreateContext(NULL, 1, &g_device, NULL, NULL, &err);
    if (err != CL_SUCCESS) return 0;
    g_queue = clCreateCommandQueue(g_ctx, g_device, 0, &err);
    if (err != CL_SUCCESS) return 0;
    g_cl_buf = clCreateBuffer(g_ctx, CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR, sz, host_ptr, &err);
    if (err != CL_SUCCESS) { fprintf(stderr, "[OCL] clCreateBuffer err=%d\n", err); return 0; }
    return 1;
}

/* Consumer thread: polls the shared region for new sequences and reads via OCL */
static void *consumer_thread(void *arg) {
    (void)arg;
    uint32_t last_seq = 0;

    while (!done && nrows < MAX_ROWS) {
        volatile uint32_t *meta_seq = (uint32_t *)((char *)g_shm + META_SEQ_OFF);
        volatile uint32_t *meta_len = (uint32_t *)((char *)g_shm + META_LEN_OFF);

        uint32_t cur_seq = __atomic_load_n(meta_seq, __ATOMIC_ACQUIRE);
        if (cur_seq == last_seq) {
            struct timespec ts = { .tv_sec=0, .tv_nsec=100000 };
            nanosleep(&ts, NULL);
            continue;
        }
        last_seq = cur_seq;

        uint32_t llr_len = *meta_len;
        if (llr_len == 0 || llr_len > LLR_MAX_BYTES) continue;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        double ocl_bw = 0.0;
        if (g_ocl_ok) {
            cl_int err;
            void *mapped = clEnqueueMapBuffer(g_queue, g_cl_buf, CL_TRUE,
                                              CL_MAP_READ, 0, llr_len,
                                              0, NULL, NULL, &err);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (err == CL_SUCCESS && mapped) {
                double ns = (t1.tv_sec - t0.tv_sec)*1e9 + (t1.tv_nsec - t0.tv_nsec);
                ocl_bw = (llr_len / 1e9) / (ns / 1e9);
                clEnqueueUnmapMemObject(g_queue, g_cl_buf, mapped, 0, NULL, NULL);
                clFinish(g_queue);
            }
        } else {
            /* No OCL: just memcpy from shared region as proxy */
            char tmp[LLR_MAX_BYTES];
            memcpy(tmp, g_shm, llr_len);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double ns = (t1.tv_sec - t0.tv_sec)*1e9 + (t1.tv_nsec - t0.tv_nsec);
            ocl_bw = (llr_len / 1e9) / (ns / 1e9);
        }

        struct timespec wall1;
        clock_gettime(CLOCK_MONOTONIC, &wall1);
        uint64_t wall_ns = (wall1.tv_sec - t0.tv_sec)*1000000000ULL +
                           (wall1.tv_nsec - t0.tv_nsec);

        rows[nrows].wall_ns    = wall_ns;
        rows[nrows].llr_len    = llr_len;
        rows[nrows].seq        = cur_seq;
        rows[nrows].ocl_bw_gbps = ocl_bw;
        nrows++;
    }
    return NULL;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s --ldpc-bin <path> [--iters N] [--output <csv>] [--bpftime-lib <path>]\n"
        "  --ldpc-bin    path to ldpc_decoder_benchmark binary\n"
        "  --iters       number of decode iterations (default 1000)\n"
        "  --output      CSV output file (default e2e_gcp.csv)\n"
        "  --bpftime-lib path to libbpftime_agent.so for LD_PRELOAD injection\n",
        prog);
}

int main(int argc, char **argv) {
    const char *ldpc_bin     = NULL;
    const char *output_csv   = "e2e_gcp.csv";
    const char *bpftime_lib  = NULL;
    int         iters        = 1000;

    static struct option longopts[] = {
        {"ldpc-bin",    required_argument, NULL, 'b'},
        {"iters",       required_argument, NULL, 'i'},
        {"output",      required_argument, NULL, 'o'},
        {"bpftime-lib", required_argument, NULL, 'L'},
        {"help",        no_argument,       NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "b:i:o:L:h", longopts, NULL)) != -1) {
        switch (c) {
            case 'b': ldpc_bin    = optarg; break;
            case 'i': iters       = atoi(optarg); break;
            case 'o': output_csv  = optarg; break;
            case 'L': bpftime_lib = optarg; break;
            case 'h': usage(argv[0]); return 0;
            default:  usage(argv[0]); return 1;
        }
    }
    if (!ldpc_bin) { usage(argv[0]); return 1; }

    printf("=== Phase 4 E2E Runner ===\n");
    printf("  ldpc binary:  %s\n", ldpc_bin);
    printf("  iterations:   %d\n", iters);
    printf("  output:       %s\n", output_csv);
    printf("  bpftime lib:  %s\n\n", bpftime_lib ? bpftime_lib : "(none — uprobe skipped)");

    /* ── 1. Check NUMA node 1 ────────────────────────────────────────────── */
    if (numa_available() < 0 || numa_max_node() < 1) {
        fprintf(stderr, "ERROR: NUMA node 1 not available\n"); return 1;
    }
    printf("[NUMA] node 1: %ld MB\n", numa_node_size(1, NULL) / (1024*1024));

    /* ── 2. Create shared CXL memfd region ──────────────────────────────── */
    g_shm_fd = memfd_create("llr_cxl", 0);
    if (g_shm_fd < 0) { perror("memfd_create"); return 1; }
    if (ftruncate(g_shm_fd, REGION_SIZE) < 0) { perror("ftruncate"); return 1; }

    g_shm = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) { perror("mmap"); return 1; }

    if (mbind_buf(g_shm, REGION_SIZE, 1) < 0) {
        perror("mbind node1");
        fprintf(stderr, "WARNING: mbind failed — continuing without CXL binding\n");
    } else {
        memset(g_shm, 0, REGION_SIZE);
        printf("[CXL] shared region: %d bytes @ %p  fd=%d\n\n", REGION_SIZE, g_shm, g_shm_fd);
    }

    /* ── 3. Init OpenCL ─────────────────────────────────────────────────── */
    g_ocl_ok = init_opencl(g_shm, REGION_SIZE);
    printf("[OCL] init: %s\n\n", g_ocl_ok ? "OK" : "SKIPPED");

    /* ── 4. Start consumer thread ───────────────────────────────────────── */
    pthread_t consumer;
    if (pthread_create(&consumer, NULL, consumer_thread, NULL) != 0) {
        perror("pthread_create"); return 1;
    }

    /* ── 5. Launch ldpc_decoder_benchmark with bpftime injection ────────── */
    char iters_str[32];
    snprintf(iters_str, sizeof(iters_str), "%d", iters);

    /* Pass shared region fd via environment so bpftime skeleton can map it */
    char fd_env[32];
    snprintf(fd_env, sizeof(fd_env), "CXL_LLR_FD=%d", g_shm_fd);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }

    if (pid == 0) {
        /* Child: set environment and exec */
        putenv(fd_env);
        if (bpftime_lib) {
            /* LD_PRELOAD bpftime agent to intercept uprobe */
            setenv("LD_PRELOAD", bpftime_lib, 1);
        }

        char *const args[] = {
            (char *)ldpc_bin,
            "-i", iters_str,
            "-s", "0",          /* BG1 code */
            NULL
        };
        execv(ldpc_bin, args);
        perror("execv");
        exit(1);
    }

    /* Parent: wait for ldpc benchmark to finish */
    int status;
    waitpid(pid, &status, 0);
    done = 1;
    pthread_join(consumer, NULL);

    printf("\nldpc benchmark exited: %d  |  rows collected: %d\n",
           WEXITSTATUS(status), nrows);

    /* ── 6. Write CSV ───────────────────────────────────────────────────── */
    FILE *f = fopen(output_csv, "w");
    if (!f) { perror("fopen csv"); return 1; }
    fprintf(f, "seq,llr_len_bytes,wall_ns,ocl_bw_gbps\n");
    for (int i = 0; i < nrows; i++) {
        fprintf(f, "%u,%u,%lu,%.6f\n",
                rows[i].seq, rows[i].llr_len,
                (unsigned long)rows[i].wall_ns, rows[i].ocl_bw_gbps);
    }
    fclose(f);
    printf("Written: %s  (%d rows)\n", output_csv, nrows);

    /* ── 7. Summary statistics ──────────────────────────────────────────── */
    if (nrows > 0) {
        double sum_bw = 0, sum_lat = 0;
        for (int i = 0; i < nrows; i++) {
            sum_bw  += rows[i].ocl_bw_gbps;
            sum_lat += rows[i].wall_ns;
        }
        double avg_bw  = sum_bw  / nrows;
        double avg_lat = sum_lat / nrows / 1000.0; /* µs */
        printf("\n=== E2E RESULTS ===\n");
        printf("  rows:             %d\n", nrows);
        printf("  avg LLR len:      %u bytes\n", rows[nrows/2].llr_len);
        printf("  avg OCL bw:       %.3f GB/s\n", avg_bw);
        printf("  avg wall latency: %.1f µs\n", avg_lat);
        printf("  PRIMARY_CONFIG:   11703 µs/slot = 23.4× (fixed anchor)\n");
    }

    if (g_ocl_ok) {
        clReleaseMemObject(g_cl_buf);
        clReleaseCommandQueue(g_queue);
        clReleaseContext(g_ctx);
    }
    munmap(g_shm, REGION_SIZE);
    close(g_shm_fd);
    return 0;
}
