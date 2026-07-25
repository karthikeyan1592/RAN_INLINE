// SPDX-License-Identifier: GPL-2.0
// Phase 2/3 — LDPC interception probe (Phase 3: counters upgraded to PERCPU).
//
// TWO uprobe attachment points:
//   1) nrLDPC_coding_decoder (slot level) — slot counter
//   2) LDPCdecoder (per-CB level) — captures int8 LLR into a BPF map so the
//      consumer can read it safely without a process_vm_readv race (p_llr
//      is a local variable that disappears after LDPCdecoder returns)
//
// Maps:
//   slot_counter  — PERCPU_ARRAY[1]: race-free per-CPU slot call tally
//   slot_desc     — ARRAY[1]: latest (frame, slot, nb_TBs)
//   cb_counter    — PERCPU_ARRAY[1]: race-free per-CPU CB call tally
//   cb_desc       — ARRAY[1]: latest (BG, Z, timestamp)
//   llr_copy      — ARRAY[1]: latest LLR data, int8_t[68*384]=26112 bytes
//
// DEV-003 resolution: OAI pushes CB decode tasks to a thread pool, so
// LDPCdecoder IS called concurrently. PERCPU_ARRAY means each CPU core
// increments its own slot — no atomic op needed, no ubpf opcode-0xdb
// rejection. Consumer aggregates across all CPUs at read time.
#define BPF_NO_GLOBAL_DATA
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define LLR_BUF_BYTES  (68 * 384)   /* max: BG1, Z=384 */

struct slot_desc_t {
    __u64 timestamp_ns;
    __s32 frame;
    __s32 slot;
    __s32 nb_TBs;
    __s32 pad;
};

struct cb_desc_t {
    __u64 timestamp_ns;   /* consumer polls this to detect new data */
    __u16 Z;
    __u8  BG;
    __u8  pad[5];
};

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} slot_counter SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct slot_desc_t);
} slot_desc SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} cb_counter SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct cb_desc_t);
} cb_desc SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __s8[LLR_BUF_BYTES]);
} llr_copy SEC(".maps");

/* ---- probe 1: nrLDPC_coding_decoder ----
 * nrLDPC_slot_decoding_parameters_t offsets (verified against real OAI headers):
 *   frame  +0  (int)
 *   slot   +4  (int)
 *   nb_TBs +8  (int)
 */
SEC("uprobe//root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build/libldpc.so:nrLDPC_coding_decoder")
int BPF_UPROBE(probe_slot_decoder, void *slot_params)
{
    __u32 k = 0;

    /* update slot descriptor */
    struct slot_desc_t sd = {};
    sd.timestamp_ns = bpf_ktime_get_ns();
    bpf_probe_read(&sd.frame,   sizeof(sd.frame),   (__u8 *)slot_params + 0);
    bpf_probe_read(&sd.slot,    sizeof(sd.slot),    (__u8 *)slot_params + 4);
    bpf_probe_read(&sd.nb_TBs,  sizeof(sd.nb_TBs),  (__u8 *)slot_params + 8);
    bpf_map_update_elem(&slot_desc, &k, &sd, BPF_ANY);

    /* per-CPU increment — no atomic needed (PERCPU_ARRAY) */
    __u64 *cnt = bpf_map_lookup_elem(&slot_counter, &k);
    if (cnt) *cnt += 1;

    return 0;
}

/* ---- probe 2: LDPCdecoder ----
 * int32_t LDPCdecoder(t_nrLDPC_dec_params *p_decParams, int8_t *p_llr, uint8_t *p_out, ...)
 * t_nrLDPC_dec_params: BG @ +0 (u8), Z @ +2 (u16)
 *
 * p_llr is int8_t[N_VN_FULL*Z] — punctured VNs already zeroed, filler=127.
 * This is EXACTLY what our OpenCL kernel expects.
 * We copy it into llr_copy map to avoid the stack-lifetime race.
 */
SEC("uprobe//root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build/libldpc.so:LDPCdecoder")
int BPF_UPROBE(probe_cb_decoder, void *p_decParams, void *p_llr, void *p_out)
{
    __u32 k = 0;

    /* read BG and Z from dec_params */
    __u8  bg = 0;
    __u16 z  = 0;
    bpf_probe_read(&bg, 1, (__u8 *)p_decParams + 0);
    bpf_probe_read(&z,  2, (__u8 *)p_decParams + 2);

    /* copy LLR into the map (avoids race with caller's stack frame) */
    __s8 *dst = bpf_map_lookup_elem(&llr_copy, &k);
    if (dst) {
        __u32 nbytes = (bg == 1) ? (68 * ((__u32)z)) : (52 * ((__u32)z));
        if (nbytes > LLR_BUF_BYTES) nbytes = LLR_BUF_BYTES;
        bpf_probe_read(dst, nbytes, p_llr);
    }

    /* update descriptor AFTER copy so consumer sees a consistent {desc+data} */
    struct cb_desc_t cd = {};
    cd.BG          = bg;
    cd.Z           = z;
    cd.timestamp_ns = bpf_ktime_get_ns();
    bpf_map_update_elem(&cb_desc, &k, &cd, BPF_ANY);

    /* per-CPU increment — no atomic needed (PERCPU_ARRAY) */
    __u64 *cnt = bpf_map_lookup_elem(&cb_counter, &k);
    if (cnt) *cnt += 1;

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
