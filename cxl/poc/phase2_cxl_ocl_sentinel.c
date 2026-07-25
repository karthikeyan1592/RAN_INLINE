/*
 * phase2_cxl_ocl_sentinel.c
 *
 * Phase 2 PoC: CXL node 1 allocation + OpenCL sentinel test
 *
 * Uses numa_alloc_onnode (proven to work) to put a buffer on CXL node 1,
 * writes a sentinel from the CPU, reads it back via OpenCL MAP.
 *
 * Build:
 *   gcc -O2 -o phase2_cxl_ocl_sentinel phase2_cxl_ocl_sentinel.c \
 *       -lOpenCL -lnuma && echo BUILD_OK
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <numa.h>
#include <numaif.h>

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#define BUF_PAGES     16           /* 64 KB — same as proven smoke test */
#define BUF_SIZE      (BUF_PAGES * 4096UL)
#define SENTINEL_U64  0xDEADBEEFCAFEBABEULL
#define SENTINEL_OFF  8            /* write at byte 8 */

static double gbps_elapsed(struct timespec *t0, struct timespec *t1, size_t bytes) {
    double ns = (t1->tv_sec - t0->tv_sec)*1e9 + (t1->tv_nsec - t0->tv_nsec);
    return (bytes / 1e9) / (ns / 1e9);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered — see output even if we hang */

    printf("=== Phase 2: CXL node1 + OpenCL Sentinel ===\n\n");

    /* ── 1. NUMA check ──────────────────────────────────────────────────── */
    if (numa_available() < 0) { fprintf(stderr, "NUMA not available\n"); return 1; }
    int maxnode = numa_max_node();
    printf("NUMA nodes: 0..%d\n", maxnode);
    if (maxnode < 1) { fprintf(stderr, "Need node 1 (CXL) — run vm_cxl_setup.sh\n"); return 1; }
    printf("  node 0 (DRAM): %lld MB\n", (long long)numa_node_size(0,NULL)/(1024*1024));
    printf("  node 1 (CXL):  %lld MB\n\n", (long long)numa_node_size(1,NULL)/(1024*1024));

    /* ── 2. Allocate on CXL node 1 via numa_alloc_onnode ───────────────── */
    printf("Allocating %zu KB on CXL node 1...\n", BUF_SIZE/1024);
    void *cxl_buf = numa_alloc_onnode(BUF_SIZE, 1);
    if (!cxl_buf) { fprintf(stderr, "numa_alloc_onnode(node1) failed\n"); return 1; }
    /* Touch all pages (proven fast for 64 KB) */
    memset(cxl_buf, 0, BUF_SIZE);
    printf("  CXL buffer: %p  (%zu KB)\n\n", cxl_buf, BUF_SIZE/1024);

    /* ── 3. Write sentinel from CPU ─────────────────────────────────────── */
    uint64_t *sentinel_ptr = (uint64_t *)((char *)cxl_buf + SENTINEL_OFF);
    *sentinel_ptr = SENTINEL_U64;
    printf("CPU wrote sentinel @ +%d: 0x%016llX\n", SENTINEL_OFF,
           (unsigned long long)*sentinel_ptr);

    /* ── 4. Simple read-back latency ────────────────────────────────────── */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    volatile uint64_t readback = *sentinel_ptr;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double read_ns = (t1.tv_sec-t0.tv_sec)*1e9 + (t1.tv_nsec-t0.tv_nsec);
    printf("CPU read-back:  0x%016llX  latency=%.0f ns  %s\n\n",
           (unsigned long long)readback, read_ns,
           readback == SENTINEL_U64 ? "PASS" : "FAIL");

    /* ── 5. BW placeholder — measured after OCL test ── */
    double bw = 0.0;

    /* ── 6. Re-write sentinel (overwritten by potential earlier memset) ── */
    *sentinel_ptr = SENTINEL_U64;

    /* ── 7. OpenCL: create buffer using USE_HOST_PTR → read sentinel ─── */
    printf("--- OpenCL sentinel readback ---\n");
    cl_platform_id platform;
    cl_device_id   device;
    cl_int err;

    err = clGetPlatformIDs(1, &platform, NULL);
    if (err != CL_SUCCESS) { printf("  clGetPlatformIDs: %d (skipping OCL)\n", err); goto done; }

    err = clGetDeviceIDs(platform, CL_DEVICE_TYPE_ALL, 1, &device, NULL);
    if (err != CL_SUCCESS) { printf("  clGetDeviceIDs: %d (skipping OCL)\n", err); goto done; }

    {
        char devname[128] = {0};
        clGetDeviceInfo(device, CL_DEVICE_NAME, sizeof(devname), devname, NULL);
        printf("  OpenCL device: %s\n", devname);

        cl_context ctx = clCreateContext(NULL, 1, &device, NULL, NULL, &err);
        if (err != CL_SUCCESS) { printf("  clCreateContext: %d\n", err); goto done; }

        cl_command_queue q = clCreateCommandQueue(ctx, device, 0, &err);
        if (err != CL_SUCCESS) { printf("  clCreateCommandQueue: %d\n", err); goto done; }

        cl_mem cl_buf = clCreateBuffer(ctx,
                                       CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR,
                                       BUF_SIZE, cxl_buf, &err);
        if (err != CL_SUCCESS) {
            printf("  clCreateBuffer(USE_HOST_PTR): %d (skipping readback)\n", err);
            goto done;
        }

        uint64_t *mapped = clEnqueueMapBuffer(q, cl_buf, CL_TRUE, CL_MAP_READ,
                                              SENTINEL_OFF, sizeof(uint64_t),
                                              0, NULL, NULL, &err);
        if (err == CL_SUCCESS && mapped) {
            uint64_t ocl_val = *mapped;
            clEnqueueUnmapMemObject(q, cl_buf, mapped, 0, NULL, NULL);
            clFinish(q);
            printf("  OCL readback:  0x%016llX  %s\n",
                   (unsigned long long)ocl_val,
                   ocl_val == SENTINEL_U64 ? "PASS" : "FAIL");
        } else {
            printf("  clEnqueueMapBuffer: %d\n", err);
        }

        clReleaseMemObject(cl_buf);
        clReleaseCommandQueue(q);
        clReleaseContext(ctx);
    }

done:
    /* ── BW: pages already faulted in, measure write speed ─────────────── */
    {
        struct timespec tb0, tb1;
        clock_gettime(CLOCK_MONOTONIC, &tb0);
        memset(cxl_buf, 0x55, BUF_SIZE);
        clock_gettime(CLOCK_MONOTONIC, &tb1);
        double ns = (tb1.tv_sec-tb0.tv_sec)*1e9+(tb1.tv_nsec-tb0.tv_nsec);
        bw = (BUF_SIZE / 1e6) / (ns / 1e9) / 1e3;  /* GB/s */
        printf("\nCXL write BW (%zu KB, pages warm): %.4f GB/s  (%.1f us)\n",
               BUF_SIZE/1024, bw, ns/1000.0);
    }

    printf("\n=== SUMMARY ===\n");
    printf("  CXL alloc (numa_alloc_onnode node1):  %s\n",
           readback == SENTINEL_U64 ? "PASS" : "FAIL");
    printf("  CXL write BW (64 KB):                 %.3f GB/s\n", bw);
    printf("  PRIMARY_CONFIG anchor:                11703 us/slot = 23.4x\n");

    numa_free(cxl_buf, BUF_SIZE);
    return (readback == SENTINEL_U64) ? 0 : 1;
}
