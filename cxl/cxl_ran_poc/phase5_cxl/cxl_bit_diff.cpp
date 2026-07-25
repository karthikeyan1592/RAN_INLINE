/*
 * cxl_bit_diff.cpp — Phase 2 (v5) Gate 2: bit-exactness routed through CXL region.
 *
 * Mirrors v4's bit_diff_test.cpp BUT routes LLR and decoded output through
 * cxl_region (cxl_base + offset) with CL_MEM_USE_HOST_PTR so the OpenCL
 * kernel reads/writes the mapped region in-place.
 *
 * Two checks:
 *  A) SENTINEL TEST — detects whether PoCL truly uses the host pointer or silently copies.
 *  B) BIT-DIFF TEST — encodes random messages via srsRAN, writes LLR to cxl region,
 *     decodes via OpenCL kernel, reads decoded bits from cxl region, compares.
 *
 * Writes: paper/results/bit_correctness_cxlpath.csv
 */

#define CL_TARGET_OPENCL_VERSION 200
#include <CL/cl.h>

#include "bg_tables.h"   /* must be first (defines static arrays) */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

/* srsRAN encoder oracle */
#include "srsran/phy/upper/channel_coding/channel_coding_factories.h"
#include "srsran/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "srsran/srsvec/bit.h"
#include "srsran/support/srsran_test.h"

/* CXL region seam */
extern "C" {
#include "cxl_region.h"
}

using namespace srsran;
using namespace srsran::ldpc;

/* ---- constants ---------------------------------------------------------- */

static constexpr int8_t  LLRS_AMPL  = 10;
static constexpr size_t  CXL_LLR_OFF = CXL_LLR_OFFSET;    /* 0 */
static constexpr size_t  CXL_OUT_OFF = CXL_OUT_OFFSET;     /* 128 MiB */
#define RESULTS_CSV  "../paper/results/bit_correctness_cxlpath.csv"
#define LDPC_CL_PATH "../gpu_daemon/ldpc_cl/ldpc_decode.cl"

/* ---- helpers ------------------------------------------------------------ */

static void ocl_die(cl_int err, const char* where)
{
    fprintf(stderr, "OpenCL error %d at %s\n", err, where);
    exit(1);
}

static std::string read_file(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) { fprintf(stderr, "Cannot open: %s\n", path); exit(1); }
    return { std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>() };
}

/* ---- test cases (same subset as v4 Gate 1) ------------------------------ */

struct test_case {
    int bg; unsigned ls; unsigned n_vn_full; unsigned n_cn;
    unsigned bg_K; unsigned n_short; unsigned ls_idx;
    const unsigned short* shifts;
};

static const test_case cases[] = {
    { 1, 384, 68, 46, 22, 66, (unsigned)LS_TO_IDX[384], &BG1_SHIFTS[LS_TO_IDX[384]][0][0] },
    { 2, 384, 52, 42, 10, 50, (unsigned)LS_TO_IDX[384], &BG2_SHIFTS[LS_TO_IDX[384]][0][0] },
    { 1, 256, 68, 46, 22, 66, (unsigned)LS_TO_IDX[256], &BG1_SHIFTS[LS_TO_IDX[256]][0][0] },
    { 2, 256, 52, 42, 10, 50, (unsigned)LS_TO_IDX[256], &BG2_SHIFTS[LS_TO_IDX[256]][0][0] },
};

/* ---- sentinel test ------------------------------------------------------ */
/*
 * Writes a pattern to cxl_base at known offset WITHOUT clEnqueueWriteBuffer.
 * Creates CL_MEM_USE_HOST_PTR buffer over the region.
 * Runs a tiny copy kernel: out_buf[0] = llr_buf[llr_off].
 * After clFinish: reads cxl_base+out_off WITHOUT clEnqueueReadBuffer.
 * If the value matches: zero-copy CONFIRMED.
 */
static const char* SENTINEL_SRC =
    "__kernel void sentinel_echo(__global const char* llr, __global char* out,\n"
    "                             int off) {\n"
    "    if (get_global_id(0) == 0) out[0] = llr[off];\n"
    "}\n";

static bool run_sentinel_test(cl_context ctx, cl_command_queue queue,
                               void* cxl_base, size_t region_sz)
{
    printf("\n[sentinel] Testing CL_MEM_USE_HOST_PTR zero-copy...\n");

    /* Write sentinel value to cxl_base[7] (no clEnqueueWriteBuffer) */
    const char SENTINEL = (char)0xBE;
    ((char*)cxl_base)[CXL_LLR_OFF + 7] = SENTINEL;
    ((char*)cxl_base)[CXL_OUT_OFF]      = 0;

    /* Build sentinel kernel */
    cl_int err;
    const char* src = SENTINEL_SRC;
    size_t slen = strlen(SENTINEL_SRC);
    cl_program prog = clCreateProgramWithSource(ctx, 1, &src, &slen, &err);
    if (err) ocl_die(err, "sentinel prog");

    cl_device_id sdev;
    clGetContextInfo(ctx, CL_CONTEXT_DEVICES, sizeof(sdev), &sdev, nullptr);

    err = clBuildProgram(prog, 0, nullptr, "-cl-std=CL1.2", nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t loglen = 0;
        clGetProgramBuildInfo(prog, sdev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &loglen);
        std::string log(loglen, ' ');
        clGetProgramBuildInfo(prog, sdev, CL_PROGRAM_BUILD_LOG, loglen, &log[0], nullptr);
        fprintf(stderr, "[sentinel] build log:\n%s\n", log.c_str());
        return false;
    }
    cl_kernel kern = clCreateKernel(prog, "sentinel_echo", &err);
    if (err) ocl_die(err, "sentinel kernel");

    /* CL_MEM_USE_HOST_PTR over two non-overlapping 4096-byte slices inside CXL region.
     * PoCL segfaults on huge USE_HOST_PTR buffers (>~8 KiB observed); small slices
     * still prove zero-copy: the host pointer IS inside the cxl mmap. */
    static const size_t SLICE = 4096;
    cl_mem llr_buf = clCreateBuffer(ctx,
        CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
        SLICE, (char*)cxl_base + 0, &err);
    if (err) { return false; }

    cl_mem out_buf = clCreateBuffer(ctx,
        CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
        SLICE, (char*)cxl_base + SLICE, &err);
    if (err) { clReleaseMemObject(llr_buf); return false; }

    int off = 7;
    clSetKernelArg(kern, 0, sizeof(cl_mem), &llr_buf);
    clSetKernelArg(kern, 1, sizeof(cl_mem), &out_buf);
    clSetKernelArg(kern, 2, sizeof(int),    &off);

    size_t gws = 1;
    err = clEnqueueNDRangeKernel(queue, kern, 1, nullptr, &gws, nullptr, 0, nullptr, nullptr);
    if (err) ocl_die(err, "sentinel enqueue");
    clFinish(queue);

    /* Read from CPU side WITHOUT clEnqueueReadBuffer.
     * out_buf maps [cxl_base+SLICE, cxl_base+2*SLICE), kernel wrote to out_buf[0]. */
    char result = ((char*)cxl_base)[SLICE];

    bool zero_copy = (result == SENTINEL);
    printf("[sentinel] wrote 0x%02X to cxl_base[7] (in llr_buf slice)\n", (unsigned char)SENTINEL);
    printf("[sentinel] read  0x%02X from cxl_base[SLICE+0] (in out_buf slice, no clEnqueueReadBuffer)\n", (unsigned char)result);
    printf("[sentinel] zero-copy: %s\n",
           zero_copy ? "CONFIRMED (CPU writes visible to kernel; kernel writes visible to CPU)"
                     : "NOT CONFIRMED — PoCL copied internally (CPU artifact, not a bug in architecture)");

    clReleaseMemObject(llr_buf);
    clReleaseMemObject(out_buf);
    clReleaseKernel(kern);
    clReleaseProgram(prog);
    return zero_copy;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char** argv)
{
    unsigned nof_messages = 10;
    unsigned n_iter       = 6;
    if (argc > 1) nof_messages = (unsigned)std::stoul(argv[1]);
    if (argc > 2) n_iter       = (unsigned)std::stoul(argv[2]);

    /* Map CXL region (stand-in on WSL2) */
    uintptr_t phys_hint = 0;
    void* cxl_base = cxl_region_map(CXL_REGION_SIZE, &phys_hint);
    printf("[cxl_bit_diff] CXL backing: %s  base=%p  size=%zu MiB\n",
           cxl_region_backing_path(), cxl_base, CXL_REGION_SIZE >> 20);
    assert((uintptr_t)cxl_base % 4096 == 0);

    /* OpenCL init */
    cl_int err;
    cl_platform_id plat;
    cl_uint n_plat = 0;
    clGetPlatformIDs(1, &plat, &n_plat);
    if (!n_plat) { fprintf(stderr, "No OpenCL platform\n"); return 1; }

    cl_device_id dev;
    clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 1, &dev, nullptr);
    char dev_name[256] = {};
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dev_name), dev_name, nullptr);
    printf("[cxl_bit_diff] OpenCL device: %s\n", dev_name);

    cl_context       ctx   = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    cl_command_queue queue = clCreateCommandQueue(ctx, dev, 0, &err);
#pragma GCC diagnostic pop

    /* Sentinel test FIRST */
    bool zero_copy = run_sentinel_test(ctx, queue, cxl_base, CXL_REGION_SIZE);

    /* Build LDPC kernel */
    std::string kl_src = read_file(LDPC_CL_PATH);
    const char* src = kl_src.c_str();
    size_t slen = kl_src.size();
    cl_program prog = clCreateProgramWithSource(ctx, 1, &src, &slen, &err);
    if (err) ocl_die(err, "clCreateProgramWithSource");

    const char* opts =
        "-DBG1_M=46 -DBG1_N=68 -DBG2_M=42 -DBG2_N=52 "
        "-DNO_EDGE=0xffff -DMAX_LS=384 -cl-std=CL1.2";
    err = clBuildProgram(prog, 1, &dev, opts, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        size_t loglen = 0;
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, 0, nullptr, &loglen);
        std::string log(loglen, ' ');
        clGetProgramBuildInfo(prog, dev, CL_PROGRAM_BUILD_LOG, loglen, &log[0], nullptr);
        fprintf(stderr, "Build log:\n%s\n", log.c_str());
        return 1;
    }
    cl_kernel kern = clCreateKernel(prog, "ldpc_decode", &err);
    if (err) ocl_die(err, "clCreateKernel");

    /* srsRAN encoder */
    auto enc_factory = create_ldpc_encoder_factory_sw("generic");
    TESTASSERT(enc_factory);
    auto encoder = enc_factory->create();
    TESTASSERT(encoder);

    /* CSV output */
    FILE* csv = fopen(RESULTS_CSV, "w");
    if (!csv) { fprintf(stderr, "Cannot create %s\n", RESULTS_CSV); exit(1); }
    fprintf(csv, "bg,ls,ls_idx,n_iter,n_messages,n_bits,n_mismatches,bit_diff_rate,status,"
                 "zero_copy_confirmed,cxl_path\n");

    std::mt19937 rgen(42);
    bool all_pass = true;

    for (auto& tc : cases) {
        unsigned msg_bits   = tc.bg_K    * tc.ls;
        unsigned short_bits = tc.n_short  * tc.ls;
        unsigned full_bits  = tc.n_vn_full * tc.ls;
        unsigned out_bytes  = (msg_bits + 7) / 8;

        size_t shift_bytes = tc.n_cn * tc.n_vn_full * sizeof(unsigned short);
        size_t c2v_bytes   = (size_t)tc.n_cn * tc.n_vn_full * tc.ls;

        /* Shift table — COPY_HOST_PTR (constant, not in CXL region) */
        cl_mem cl_shifts = clCreateBuffer(ctx,
            CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
            shift_bytes, (void*)tc.shifts, &err);
        if (err) ocl_die(err, "cl_shifts");

        /*
         * LLR buffer — CL_MEM_USE_HOST_PTR over cxl_base + CXL_LLR_OFF.
         * Must be size-aligned to 64 bytes (PoCL requirement); full_bits already even.
         * If PoCL silently copies, sentinel test caught it above.
         */
        size_t llr_sz = ((full_bits * sizeof(int8_t) + 63) / 64) * 64;
        cl_mem cl_llr = clCreateBuffer(ctx,
            CL_MEM_USE_HOST_PTR | CL_MEM_READ_ONLY,
            llr_sz, (char*)cxl_base + CXL_LLR_OFF, &err);
        if (err) ocl_die(err, "cl_llr USE_HOST_PTR");

        /* Out buffer — CL_MEM_USE_HOST_PTR over cxl_base + CXL_OUT_OFF */
        size_t out_sz = ((out_bytes + 63) / 64) * 64;
        cl_mem cl_out = clCreateBuffer(ctx,
            CL_MEM_USE_HOST_PTR | CL_MEM_WRITE_ONLY,
            out_sz, (char*)cxl_base + CXL_OUT_OFF, &err);
        if (err) ocl_die(err, "cl_out USE_HOST_PTR");

        /* c2v — regular device buffer (decoder state, not on CXL) */
        cl_mem cl_c2v = clCreateBuffer(ctx, CL_MEM_READ_WRITE, c2v_bytes, nullptr, &err);
        if (err) ocl_die(err, "cl_c2v");
        std::vector<int8_t> c2v_zeros(c2v_bytes, 0);

        printf("\nBG%d LS=%u ls_idx=%u  msg=%u bits  codeword=%u bits  n_iter=%u\n",
               tc.bg, tc.ls, tc.ls_idx, msg_bits, short_bits, n_iter);

        long total_mismatches = 0, total_bits = 0;

        for (unsigned i_msg = 0; i_msg < nof_messages; i_msg++) {
            /* 1. Random message */
            dynamic_bit_buffer msg_buf(msg_bits);
            for (unsigned i = 0; i < msg_bits; i++)
                msg_buf.insert((uint8_t)(rgen() & 1), i, 1);

            /* 2. Encode */
            codeblock_metadata::tb_common_metadata enc_cfg;
            enc_cfg.base_graph   = (tc.bg == 1) ? ldpc_base_graph_type::BG1
                                                 : ldpc_base_graph_type::BG2;
            enc_cfg.lifting_size = static_cast<lifting_size_t>(tc.ls);
            const ldpc_encoder_buffer& rm_buf = encoder->encode(msg_buf, enc_cfg);

            std::vector<uint8_t> codeword(short_bits, 0);
            rm_buf.write_codeblock(codeword, 0);

            /* 3. Build full LLR and WRITE TO CXL REGION (zero-copy path) */
            auto* llr_ptr = (int8_t*)((char*)cxl_base + CXL_LLR_OFF);
            memset(llr_ptr, 0, full_bits);
            unsigned punct_off = 2 * tc.ls;
            for (unsigned j = 0; j < short_bits; j++)
                llr_ptr[punct_off + j] = (codeword[j] == 0) ? LLRS_AMPL : -LLRS_AMPL;

            /* Clear output side of CXL region */
            auto* out_ptr = (uint8_t*)((char*)cxl_base + CXL_OUT_OFF);
            memset(out_ptr, 0, out_bytes);

            /* 4. If PoCL copies internally, we must explicitly upload */
            if (!zero_copy) {
                err = clEnqueueWriteBuffer(queue, cl_llr, CL_TRUE, 0,
                    full_bits * sizeof(int8_t), llr_ptr, 0, nullptr, nullptr);
                if (err) ocl_die(err, "WriteBuffer llr (PoCL-copy fallback)");
                err = clEnqueueWriteBuffer(queue, cl_out, CL_TRUE, 0,
                    out_bytes, out_ptr, 0, nullptr, nullptr);
                if (err) ocl_die(err, "WriteBuffer out (PoCL-copy fallback)");
            }

            /* Reset c2v state */
            err = clEnqueueWriteBuffer(queue, cl_c2v, CL_TRUE, 0,
                c2v_bytes, c2v_zeros.data(), 0, nullptr, nullptr);
            if (err) ocl_die(err, "WriteBuffer c2v");

            /* 5. Set kernel args and enqueue.
             * Arg order (matches kernel signature exactly):
             *   0: llr_input  1: bit_output  2: bg_shifts  3: c2v_buf
             *   4: n_vn_full  5: n_cn  6: n_vn_info  7: ls  8: n_iter  9: cb_offset */
            cl_int n_vn_full_a = (cl_int)tc.n_vn_full;
            cl_int n_cn_a      = (cl_int)tc.n_cn;
            cl_int n_vn_info_a = (cl_int)tc.bg_K;   /* 22 (BG1) or 10 (BG2) */
            cl_int ls_a        = (cl_int)tc.ls;      /* lifting size */
            cl_int n_iter_a    = (cl_int)n_iter;
            cl_int cb_off_a    = 0;                  /* single codeblock */

            clSetKernelArg(kern, 0, sizeof(cl_mem), &cl_llr);
            clSetKernelArg(kern, 1, sizeof(cl_mem), &cl_out);
            clSetKernelArg(kern, 2, sizeof(cl_mem), &cl_shifts);
            clSetKernelArg(kern, 3, sizeof(cl_mem), &cl_c2v);
            clSetKernelArg(kern, 4, sizeof(cl_int), &n_vn_full_a);
            clSetKernelArg(kern, 5, sizeof(cl_int), &n_cn_a);
            clSetKernelArg(kern, 6, sizeof(cl_int), &n_vn_info_a);
            clSetKernelArg(kern, 7, sizeof(cl_int), &ls_a);
            clSetKernelArg(kern, 8, sizeof(cl_int), &n_iter_a);
            clSetKernelArg(kern, 9, sizeof(cl_int), &cb_off_a);

            size_t gws = 1;
            err = clEnqueueNDRangeKernel(queue, kern, 1, nullptr, &gws, nullptr,
                                          0, nullptr, nullptr);
            if (err) ocl_die(err, "NDRange ldpc_decode");
            clFinish(queue);

            /* 6. Read decoded bits from CXL region (zero-copy: already there) */
            if (!zero_copy) {
                err = clEnqueueReadBuffer(queue, cl_out, CL_TRUE, 0,
                    out_bytes, out_ptr, 0, nullptr, nullptr);
                if (err) ocl_die(err, "ReadBuffer out (PoCL-copy fallback)");
            }
            /* CPU reads directly from cxl_base + CXL_OUT_OFF */

            /* 7. Compare decoded bits vs message */
            long mismatches = 0;
            for (unsigned i = 0; i < msg_bits; i++) {
                unsigned byte_idx = i / 8;
                unsigned bit_pos  = 7 - (i % 8);
                uint8_t dec_bit   = (out_ptr[byte_idx] >> bit_pos) & 1;
                uint8_t msg_bit   = (uint8_t)msg_buf.extract(i, 1);
                if (dec_bit != msg_bit) mismatches++;
            }
            total_mismatches += mismatches;
            total_bits       += msg_bits;
        }

        double rate = total_bits ? (double)total_mismatches / total_bits : 0.0;
        const char* status = (total_mismatches == 0) ? "PASS" : "FAIL";
        if (total_mismatches) all_pass = false;

        printf("  mismatches=%ld/%ld  rate=%.6f  %s\n",
               total_mismatches, total_bits, rate, status);

        fprintf(csv, "%d,%u,%u,%u,%u,%ld,%ld,%.6f,%s,%s,%s\n",
                tc.bg, tc.ls, tc.ls_idx, n_iter, nof_messages,
                total_bits, total_mismatches, rate, status,
                zero_copy ? "YES" : "NO (PoCL-copy)",
                cxl_region_backing_path());

        clReleaseMemObject(cl_shifts);
        clReleaseMemObject(cl_llr);
        clReleaseMemObject(cl_out);
        clReleaseMemObject(cl_c2v);
    }

    fclose(csv);
    printf("\n[cxl_bit_diff] zero_copy=%s\n", zero_copy ? "CONFIRMED" : "PoCL-copy (CPU artifact)");
    printf("[cxl_bit_diff] all_pass=%s\n", all_pass ? "YES" : "NO");
    printf("[cxl_bit_diff] Results: %s\n", RESULTS_CSV);

    clReleaseKernel(kern);
    clReleaseProgram(prog);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    cxl_region_unmap(cxl_base, CXL_REGION_SIZE);

    return all_pass ? 0 : 1;
}
