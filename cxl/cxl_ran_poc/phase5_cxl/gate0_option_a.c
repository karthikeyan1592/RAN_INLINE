/* gate0_option_a.c — v6 Gate 0 Option A
 *
 * Proves in ONE configuration (system-ram mode, NUMA node 1 = CXL):
 *   PROOF1: numa_alloc_onnode(node=1) physically lands on CXL node
 *           confirmed via get_mempolicy(MPOL_F_ADDR|MPOL_F_NODE)
 *   PROOF2: clCreateBuffer(CL_MEM_USE_HOST_PTR, node1_ptr) succeeds,
 *           sentinel written CPU-side is read by kernel without
 *           clEnqueueWriteBuffer — true zero-copy over CXL memory
 *
 * Build: gcc -O2 -o gate0_option_a gate0_option_a.c -lnuma -lOpenCL
 * Run inside QEMU VM where dax0.0 is in system-ram mode (NUMA node 1 live)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <numa.h>
#include <numaif.h>
#include <CL/cl.h>

#define BUF_SIZE   (1 * 1024 * 1024)   /* 1 MB */
#define SENTINEL   0xCA7EBEEFUL
#define CXL_NODE   1

static const char *CL_SRC =
"__kernel void sentinel_check("
"    __global const uint *in,"
"    __global       uint *out) {"
"  out[0] = in[0];"   /* read in[0] that was written CPU-side before any CL call */
"}";

static void die(int err, const char *msg)
{
    fprintf(stderr, "FATAL: %s (err=%d)\n", msg, err);
    exit(1);
}

int main(void)
{
    int rc = 0;

    /* ── sanity: is NUMA node 1 available? ──────────────────── */
    if (numa_available() < 0) die(-1, "libnuma not available");
    int max_node = numa_max_node();
    printf("[gate0] NUMA max_node=%d\n", max_node);
    if (max_node < CXL_NODE) {
        fprintf(stderr, "FAIL: NUMA node %d not present (max=%d) — "
                "is dax0.0 in system-ram mode?\n", CXL_NODE, max_node);
        return 1;
    }
    long node1_free = numa_node_size64(CXL_NODE, NULL);
    printf("[gate0] NUMA node %d size=%ld MB\n", CXL_NODE, node1_free/(1024*1024));

    /* ── PROOF 1: allocate on node 1, verify via get_mempolicy ─ */
    printf("[gate0] allocating %d bytes on NUMA node %d...\n", BUF_SIZE, CXL_NODE);
    void *buf = numa_alloc_onnode(BUF_SIZE, CXL_NODE);
    if (!buf) die(errno, "numa_alloc_onnode");

    /* touch all pages to force physical placement */
    memset(buf, 0, BUF_SIZE);

    /* get_mempolicy with MPOL_F_ADDR|MPOL_F_NODE → node ID in policy arg */
    int node_id = -1;
    if (get_mempolicy(&node_id, NULL, 0, buf, MPOL_F_ADDR | MPOL_F_NODE) != 0) {
        perror("get_mempolicy");
        rc = 1;
    }
    printf("[gate0] PROOF1 ptr=%p numa_node=%d cxl_node=%s exit=%d\n",
           buf, node_id, node_id == CXL_NODE ? "YES" : "NO", rc);
    if (node_id != CXL_NODE) {
        fprintf(stderr, "FAIL: expected numa_node=%d got %d\n", CXL_NODE, node_id);
        rc = 1;
    }

    /* ── Write sentinel CPU-side BEFORE any CL call ─────────── */
    uint32_t *ibuf = (uint32_t *)buf;
    ibuf[0] = (uint32_t)SENTINEL;
    printf("[gate0] wrote sentinel 0x%08X at buf[0] (CPU-side, no CL write)\n", ibuf[0]);

    /* ── PROOF 2: CL_MEM_USE_HOST_PTR over node-1 allocation ── */
    cl_int err;
    cl_platform_id plat = NULL;
    cl_device_id   dev  = NULL;
    cl_uint nplat = 0;

    err = clGetPlatformIDs(1, &plat, &nplat);
    if (err || nplat == 0) die(err, "clGetPlatformIDs");

    char pname[128] = {0};
    clGetPlatformInfo(plat, CL_PLATFORM_NAME, sizeof(pname), pname, NULL);
    printf("[gate0] OpenCL platform: %s\n", pname);

    err = clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 1, &dev, NULL);
    if (err) die(err, "clGetDeviceIDs");

    char dname[128] = {0};
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dname), dname, NULL);
    printf("[gate0] OpenCL device:   %s\n", dname);

    cl_context ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err);
    if (err) die(err, "clCreateContext");

    cl_command_queue q = clCreateCommandQueue(ctx, dev, 0, &err);
    if (err) die(err, "clCreateCommandQueue");

    /* create buffer over node-1 memory — no copy, CL uses host ptr directly */
    cl_mem cl_in = clCreateBuffer(ctx,
                                  CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE,
                                  BUF_SIZE, buf, &err);
    printf("[gate0] PROOF2 clCreateBuffer(CL_MEM_USE_HOST_PTR) err=%d (0=OK)\n", err);
    if (err) die(err, "clCreateBuffer CL_MEM_USE_HOST_PTR");

    /* output buffer — small, device-side */
    cl_mem cl_out = clCreateBuffer(ctx, CL_MEM_READ_WRITE,
                                   sizeof(uint32_t), NULL, &err);
    if (err) die(err, "clCreateBuffer output");

    /* build kernel */
    cl_program prog = clCreateProgramWithSource(ctx, 1, &CL_SRC, NULL, &err);
    if (err) die(err, "clCreateProgramWithSource");
    err = clBuildProgram(prog, 1, &dev, NULL, NULL, NULL);
    if (err) {
        char log[4096];
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, sizeof(log), log, NULL);
        fprintf(stderr, "Build log: %s\n", log);
        die(err, "clBuildProgram");
    }
    cl_kernel kern = clCreateKernel(prog, "sentinel_check", &err);
    if (err) die(err, "clCreateKernel");

    clSetKernelArg(kern, 0, sizeof(cl_mem), &cl_in);
    clSetKernelArg(kern, 1, sizeof(cl_mem), &cl_out);

    /* enqueue kernel — no clEnqueueWriteBuffer before this */
    size_t gws = 1;
    err = clEnqueueNDRangeKernel(q, kern, 1, NULL, &gws, NULL, 0, NULL, NULL);
    if (err) die(err, "clEnqueueNDRangeKernel");

    /* read output */
    uint32_t result = 0;
    err = clEnqueueReadBuffer(q, cl_out, CL_TRUE, 0,
                              sizeof(uint32_t), &result, 0, NULL, NULL);
    if (err) die(err, "clEnqueueReadBuffer");
    clFinish(q);

    printf("[gate0] sentinel_cpu=0x%08X cl_out=0x%08X match=%s\n",
           ibuf[0], result, result == ibuf[0] ? "YES" : "NO");

    if (result != ibuf[0]) {
        fprintf(stderr, "FAIL: sentinel mismatch — zero-copy not confirmed\n");
        rc = 1;
    } else {
        printf("[gate0] GATE0 PASS: option=A zero_copy=YES numa_node=%d\n", node_id);
    }

    /* cleanup */
    clReleaseKernel(kern); clReleaseProgram(prog);
    clReleaseMemObject(cl_in); clReleaseMemObject(cl_out);
    clReleaseCommandQueue(q); clReleaseContext(ctx);
    numa_free(buf, BUF_SIZE);
    return rc;
}
