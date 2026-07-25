/*
 * bit_diff_test.cpp — Phase 1 Gate 1 bit-exactness verification
 *
 * PORT NOTE (p0-rig-scaffold, 2026-07-21): mechanically re-pinned from srsRAN Project (AGPLv3)
 * to OCUDU (BSD-3-Clause-Open-MPI) — include paths and the `srsran`->`ocudu` namespace only;
 * zero logic/algorithm change. See PRIOR_WORK.sha256 for the original file's checksum and
 * bit_diff_test.cpp.orig for the byte-for-byte original. Rationale: this harness's only external
 * dependency is OCUDU's LDPC ENCODER (used solely to generate oracle ground-truth messages); the
 * bit-exact KERNEL under test (ldpc_decode.cl, bg_tables.h) is unmodified, ours, and has zero
 * upstream dependency either way. Building against OCUDU's BSD-3 library instead of the old AGPL
 * srsRAN means this image carries no copyleft obligation. RESULTS_CSV path is overridden at
 * compile time (-DRESULTS_CSV=...) for the container filesystem, not edited here (P0-R6).
 *
 * Uses OCUDU's LDPC encoder to generate codewords from random messages,
 * then verifies our OpenCL min-sum decoder (ldpc_decode.cl) reproduces
 * the original message bits exactly.
 *
 * BG column layout (5G NR TS 38.212):
 *   VN0..VN1       : punctured (not transmitted). Carry user data; decoder infers
 *                    from parity equations. Input LLR = 0 (neutral, same as srsRAN).
 *   VN2..VN(K+1)  : info bits (K-2 transmitted VNs = first bits of write_codeblock output).
 *   VN(K+2)..VN65 : parity bits (transmitted).
 *   BG1: N_FULL=68, N_SHORT=66, K=22
 *   BG2: N_FULL=52, N_SHORT=50, K=10
 *
 * Output of our kernel: packed bits for VN0..VN(K-1) (K*ls bits, MSB-first).
 * These should match the original message bits at all K*ls positions.
 *
 * Writes: paper/results/bit_correctness.csv
 */

/* bg_tables.h must be included at file scope (defines static arrays) */
#include "bg_tables.h"

#include <CL/cl.h>
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

/* srsRAN includes */
#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ocudu/ran/sch/ldpc_base_graph.h"
#include "ocudu/ocuduvec/bit.h"
#include "ocudu/support/ocudu_test.h"

using namespace ocudu;
using namespace ocudu::ldpc;

static constexpr int8_t  LLRS_AMPL = 10;   /* same as srsRAN unit test */
#ifndef RESULTS_CSV
#define RESULTS_CSV  "../../paper/results/bit_correctness.csv"
#endif

static void ocl_die(cl_int err, const char* where)
{
    fprintf(stderr, "OpenCL error %d at %s\n", err, where);
    exit(1);
}

static std::string read_file(const char* path)
{
    std::ifstream f(path);
    if (!f.is_open()) { fprintf(stderr, "Cannot open: %s\n", path); exit(1); }
    return std::string(std::istreambuf_iterator<char>(f),
                       std::istreambuf_iterator<char>());
}

struct test_case {
    int      bg;         /* 1 or 2 */
    unsigned ls;         /* lifting size */
    unsigned n_vn_full;  /* 68 or 52 */
    unsigned n_cn;       /* 46 or 42 */
    unsigned bg_K;       /* 22 or 10 (info VNs including 2 punctured) */
    unsigned n_short;    /* 66 or 50 (transmitted VNs) */
    unsigned ls_idx;     /* index into BG1_SHIFTS / BG2_SHIFTS tables */
    const unsigned short* shifts; /* pointer into bg_tables.h arrays */
};

int main(int argc, char** argv)
{
    unsigned nof_messages = 10;
    unsigned n_iter       = 6;
    if (argc > 1) nof_messages = (unsigned)std::stoul(argv[1]);
    if (argc > 2) n_iter       = (unsigned)std::stoul(argv[2]);

    /* Gate 1 minimum: BG1 LS=384.  Additional cases strengthen coverage.
     * LS=384 → ls_idx=1 (LS_TO_IDX[384]=1 per bg_tables.h).
     * LS=256 → ls_idx=0 (LS_TO_IDX[256]=0).                               */
    std::vector<test_case> cases = {
        {1, 384, 68, 46, 22, 66, (unsigned)LS_TO_IDX[384], &BG1_SHIFTS[LS_TO_IDX[384]][0][0]},
        {2, 384, 52, 42, 10, 50, (unsigned)LS_TO_IDX[384], &BG2_SHIFTS[LS_TO_IDX[384]][0][0]},
        {1, 256, 68, 46, 22, 66, (unsigned)LS_TO_IDX[256], &BG1_SHIFTS[LS_TO_IDX[256]][0][0]},
        {2, 256, 52, 42, 10, 50, (unsigned)LS_TO_IDX[256], &BG2_SHIFTS[LS_TO_IDX[256]][0][0]},
    };

    /* ---- OpenCL setup ---- */
    cl_int         err;
    cl_platform_id plat = nullptr;
    cl_device_id   dev  = nullptr;
    clGetPlatformIDs(1, &plat, nullptr);
    err = clGetDeviceIDs(plat, CL_DEVICE_TYPE_CPU, 1, &dev, nullptr);
    if (err != CL_SUCCESS) clGetDeviceIDs(plat, CL_DEVICE_TYPE_ALL, 1, &dev, nullptr);

    char dev_name[256] = {};
    clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof(dev_name), dev_name, nullptr);
    printf("OpenCL device: %s\n", dev_name);

    cl_context       ctx   = clCreateContext(nullptr, 1, &dev, nullptr, nullptr, &err);
    cl_command_queue queue = clCreateCommandQueue(ctx, dev, 0, &err);

    /* Build OpenCL program from kernel source only.
     * bg_shifts are passed as a __constant buffer parameter (not compiled in). */
    std::string kl_src = read_file("ldpc_decode.cl");
    const char* src    = kl_src.c_str();
    size_t      slen   = kl_src.size();

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
        exit(1);
    }

    cl_kernel kern = clCreateKernel(prog, "ldpc_decode", &err);
    if (err) ocl_die(err, "clCreateKernel");

    /* ---- srsRAN encoder ---- */
    auto enc_factory = create_ldpc_encoder_factory_sw("generic");
    TESTASSERT(enc_factory);
    auto encoder = enc_factory->create();
    TESTASSERT(encoder);

    /* ---- CSV output ---- */
    FILE* csv = fopen(RESULTS_CSV, "w");
    if (!csv) { fprintf(stderr, "Cannot create %s\n", RESULTS_CSV); exit(1); }
    fprintf(csv, "bg,ls,ls_idx,n_iter,n_messages,n_bits,n_mismatches,bit_diff_rate,status\n");

    std::mt19937 rgen(42);
    bool all_pass = true;

    for (auto& tc : cases) {
        unsigned msg_bits     = tc.bg_K   * tc.ls;    /* K*ls */
        unsigned short_bits   = tc.n_short * tc.ls;   /* N_SHORT*ls = transmitted */
        unsigned full_bits    = tc.n_vn_full * tc.ls; /* N_FULL*ls = full VN array */
        unsigned out_bytes    = (msg_bits + 7) / 8;

        size_t shift_bytes = tc.n_cn * tc.n_vn_full * sizeof(unsigned short);
        /* c2v_buf: one int8 per (cn, vn, z) edge replica — layered decoder state */
        size_t c2v_bytes   = (size_t)tc.n_cn * tc.n_vn_full * tc.ls * sizeof(int8_t);

        /* CL buffers — recreated per test case */
        cl_mem cl_shifts = clCreateBuffer(ctx, CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
                                          shift_bytes, (void*)tc.shifts, &err);
        if (err) ocl_die(err, "cl_shifts");

        cl_mem cl_llr = clCreateBuffer(ctx, CL_MEM_READ_ONLY,
                                       full_bits * sizeof(int8_t), nullptr, &err);
        if (err) ocl_die(err, "cl_llr");

        cl_mem cl_out = clCreateBuffer(ctx, CL_MEM_WRITE_ONLY, out_bytes, nullptr, &err);
        if (err) ocl_die(err, "cl_out");

        /* c2v edge-message buffer (READ_WRITE: kernel reads old, writes new each iter) */
        cl_mem cl_c2v = clCreateBuffer(ctx, CL_MEM_READ_WRITE, c2v_bytes, nullptr, &err);
        if (err) ocl_die(err, "cl_c2v");

        /* Zero vector for resetting c2v before each message decode */
        std::vector<int8_t> c2v_zeros(c2v_bytes, 0);

        printf("Testing BG%d LS=%u ls_idx=%u  msg=%u bits  codeword=%u bits  n_iter=%u\n",
               tc.bg, tc.ls, tc.ls_idx, msg_bits, short_bits, n_iter);

        long total_mismatches = 0;
        long total_bits       = 0;

        for (unsigned i_msg = 0; i_msg < nof_messages; i_msg++) {
            /* 1. Random message */
            dynamic_bit_buffer msg_buf(msg_bits);
            for (unsigned i = 0; i < msg_bits; i++) {
                msg_buf.insert((uint8_t)(rgen() & 1), i, 1);
            }

            /* 2. Encode: returns short codeword of N_SHORT*ls bits (VN2..VN(N_FULL-1))
             * OCUDU re-pin note: ldpc_encoder::encode()'s 2nd argument type changed from the old
             * srsRAN's shared codeblock_metadata::tb_common_metadata to a dedicated
             * ldpc_encoder::configuration struct (same two fields we set; Nref defaults to 0 =
             * unlimited buffer, matching the original's unset behavior). API-surface change
             * tracked, no test logic changed. */
            ldpc_encoder::configuration enc_cfg;
            enc_cfg.base_graph   = (tc.bg == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
            enc_cfg.lifting_size = static_cast<lifting_size_t>(tc.ls);
            const ldpc_encoder_buffer& rm_buf = encoder->encode(msg_buf, enc_cfg);

            std::vector<uint8_t> codeword(short_bits, 0);
            rm_buf.write_codeblock(codeword, 0); /* offset=0: writes VN2..VN(N_FULL-1) */

            /* 3. Build full LLR array [full_bits]:
             *    VN0, VN1 (indices 0..2*ls-1): 0 (neutral, punctured — srsRAN convention)
             *    VN2..VN(N_FULL-1) (indices 2*ls..full_bits-1): ±LLRS_AMPL from codeword */
            std::vector<int8_t> llr_full(full_bits, 0);
            unsigned punct_offset = 2 * tc.ls;
            for (unsigned j = 0; j < short_bits; j++) {
                uint8_t bit = codeword[j];
                llr_full[punct_offset + j] = (bit == 0) ? LLRS_AMPL : -LLRS_AMPL;
            }

            /* 4. Upload LLR, clear output, zero c2v (decoder state must reset per message) */
            err = clEnqueueWriteBuffer(queue, cl_llr, CL_TRUE, 0,
                                       full_bits * sizeof(int8_t), llr_full.data(),
                                       0, nullptr, nullptr);
            if (err) ocl_die(err, "WriteBuffer llr");

            std::vector<uint8_t> out_host(out_bytes, 0);
            err = clEnqueueWriteBuffer(queue, cl_out, CL_TRUE, 0,
                                       out_bytes, out_host.data(), 0, nullptr, nullptr);
            if (err) ocl_die(err, "WriteBuffer out");

            /* c2v_buf must be zeroed before each decode (layered decoder state) */
            err = clEnqueueWriteBuffer(queue, cl_c2v, CL_TRUE, 0,
                                       c2v_bytes, c2v_zeros.data(), 0, nullptr, nullptr);
            if (err) ocl_die(err, "WriteBuffer c2v");

            /* 5. Run kernel (local_size=1 → serial, safe for any ls on PoCL CPU)
             * Arg order: llr_input, bit_output, bg_shifts, c2v_buf,
             *            n_vn_full, n_cn, n_vn_info, ls, n_iter, cb_offset          */
            cl_int n_vn_full_a = (cl_int)tc.n_vn_full;
            cl_int n_cn_a      = (cl_int)tc.n_cn;
            cl_int n_vn_info_a = (cl_int)tc.bg_K;  /* K=22 (BG1) or 10 (BG2) */
            cl_int ls_a        = (cl_int)tc.ls;
            cl_int n_iter_a    = (cl_int)n_iter;
            cl_int cb_off_a    = 0;

            clSetKernelArg(kern, 0, sizeof(cl_mem), &cl_llr);
            clSetKernelArg(kern, 1, sizeof(cl_mem), &cl_out);
            clSetKernelArg(kern, 2, sizeof(cl_mem), &cl_shifts);
            clSetKernelArg(kern, 3, sizeof(cl_mem), &cl_c2v);    /* NEW: c2v edge buffer */
            clSetKernelArg(kern, 4, sizeof(cl_int), &n_vn_full_a);
            clSetKernelArg(kern, 5, sizeof(cl_int), &n_cn_a);
            clSetKernelArg(kern, 6, sizeof(cl_int), &n_vn_info_a);
            clSetKernelArg(kern, 7, sizeof(cl_int), &ls_a);
            clSetKernelArg(kern, 8, sizeof(cl_int), &n_iter_a);
            clSetKernelArg(kern, 9, sizeof(cl_int), &cb_off_a);

            size_t gs = 1, ls_wg = 1;
            err = clEnqueueNDRangeKernel(queue, kern, 1, nullptr, &gs, &ls_wg, 0, nullptr, nullptr);
            if (err) ocl_die(err, "NDRange");
            clFinish(queue);

            /* 6. Read back decoded bits */
            err = clEnqueueReadBuffer(queue, cl_out, CL_TRUE, 0,
                                      out_bytes, out_host.data(), 0, nullptr, nullptr);
            if (err) ocl_die(err, "ReadBuffer out");

            /* 7. Bit-compare decoded output vs original message.
             *
             * Decoder output layout (packed MSB-first):
             *   bits 0..2*ls-1   : decoded VN0, VN1 (punctured, recovered from parity)
             *   bits 2*ls..K*ls-1: decoded VN2..VN(K-1) (systematic info bits)
             *
             * Original message layout:
             *   bits 0..2*ls-1   : VN0, VN1 values (put there by the encoder)
             *   bits 2*ls..K*ls-1: VN2..VN(K-1) info
             *
             * Both should match exactly at high SNR (LLRS_AMPL=10 >> noise floor).
             * NOTE: the punctured VN0,VN1 output depends on the LLR=0 initial value
             * and parity-equation convergence. At LLRS_AMPL=10 this is reliable.
             * We compare ALL msg_bits to be strict. */
            long mismatches = 0;
            for (unsigned b = 0; b < msg_bits; b++) {
                uint8_t msg_bit = (uint8_t)(msg_buf.extract(b, 1) & 1);
                unsigned byte_idx = b / 8;
                unsigned bit_pos  = 7 - (b % 8);
                uint8_t dec_bit   = (out_host[byte_idx] >> bit_pos) & 1u;
                if (msg_bit != dec_bit) mismatches++;
            }
            total_mismatches += mismatches;
            total_bits       += (long)msg_bits;

            if (i_msg < 3 || mismatches > 0) {
                printf("  msg[%u]: %ld/%u mismatches\n", i_msg, mismatches, msg_bits);
            }
        }

        double bit_diff = (total_bits > 0) ? (double)total_mismatches / (double)total_bits : 0.0;
        const char* verdict = (total_mismatches == 0) ? "PASS" : "FAIL";
        printf("BG%d LS=%u: %ld mismatches / %ld bits  bit_diff_rate=%.6f  [%s]\n\n",
               tc.bg, tc.ls, total_mismatches, total_bits, bit_diff, verdict);

        fprintf(csv, "%d,%u,%u,%u,%u,%ld,%ld,%.6f,%s\n",
                tc.bg, tc.ls, tc.ls_idx, n_iter, nof_messages,
                total_bits, total_mismatches, bit_diff, verdict);

        if (total_mismatches != 0) all_pass = false;

        clReleaseMemObject(cl_shifts);
        clReleaseMemObject(cl_llr);
        clReleaseMemObject(cl_out);
        clReleaseMemObject(cl_c2v);
    }

    fclose(csv);
    printf("Gate 1 overall: %s\n", all_pass ? "PASS" : "FAIL");

    clReleaseKernel(kern);
    clReleaseProgram(prog);
    clReleaseCommandQueue(queue);
    clReleaseContext(ctx);
    return all_pass ? 0 : 1;
}
