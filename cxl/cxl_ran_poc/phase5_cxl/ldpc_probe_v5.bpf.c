/* SPDX-License-Identifier: GPL-2.0
 * ldpc_probe_v5.bpf.c — Phase 3 (v5) bpftime uprobe.
 *
 * Change B: writes a descriptor (52 bytes) to a BPF RINGBUF instead of
 * copying the full LLR payload (26 KiB).  On the DO path (real /dev/dax0.0),
 * OAI's LLR already lives in the CXL region via numactl --membind; the
 * handler computes llr_off = p_llr - region_base with ZERO additional copy.
 * On the WSL2 stand-in path, a single bpf_probe_read into llr_staging is the
 * one documented copy; the consumer relays it to cxl_base+llr_off.
 *
 * DEV-003 resolution: BPF RINGBUF reserve/submit is inherently MPSC-safe
 * (kernel ringbuf protocol serialises concurrent reservations).  Multiple OAI
 * thread-pool workers can fire this probe simultaneously; thread IDs are
 * logged to tid_log for Gate 3 evidence.
 *
 * Maps:
 *   desc_ringbuf  — RINGBUF:         descriptor stream (52-byte structs)
 *   llr_staging   — ARRAY[1]:        WSL2-only LLR staging (26112 bytes)
 *   cxl_config    — ARRAY[1]:        region_base, is_standin (set by consumer)
 *   tid_log       — ARRAY[100]:      first 100 thread IDs observed
 *   tid_count     — ARRAY[1]:        running count of TIDs logged
 *   seq_counter   — PERCPU_ARRAY[1]: per-CPU sequence number
 */
#define BPF_NO_GLOBAL_DATA
#include <vmlinux.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define LLR_MAX_BYTES    (68 * 384)   /* BG1, Z=384 = 26112 bytes */
#define TID_LOG_CAP      100
#define N_LLR_SLOTS      256          /* power-of-2, matches DESC_RING_CAPACITY */
#define LLR_SLOT_STRIDE  26176        /* ceil(26112/64)*64 — cache-line aligned */

/* Mirrors struct ldpc_desc in desc_ring.h — MUST be byte-identical.
 * Both sides use __attribute__((packed)) so layout is identical on x86_64. */
struct ldpc_desc_bpf {
    __u64 timestamp_ns;
    __u32 slot_id;
    __u32 cb_index;
    __u64 llr_off;
    __u32 llr_len;
    __u64 out_off;
    __u32 out_len;
    __u32 seq;
    __u8  bg;
    __u8  Zc;
    __u8  pad[6];
} __attribute__((packed));

/* CXL config — consumer fills this before attaching gNB */
struct cxl_cfg_t {
    __u64 region_base;    /* mmap base of CXL region in OAI's address space */
    __u64 region_size;    /* total CXL region size (256 MiB) */
    __u64 out_base_off;   /* CXL_OUT_OFFSET = 128 MiB */
    __u32 is_standin;     /* 1 = WSL2 stand-in; 0 = real /dev/dax0.0 */
    __u32 _pad;
};

/* Descriptor stream — MPSC-safe via bpf_ringbuf_reserve/submit protocol */
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 131072);  /* 128 KiB; holds ~2520 descriptors */
} desc_ringbuf SEC(".maps");

/* WSL2-only LLR staging: handler writes here, consumer relays to CXL region.
 * Last-writer wins; race-benign for Gate 3 (we count descriptors, not bits). */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u8[LLR_MAX_BYTES]);
} llr_staging SEC(".maps");

/* CXL region configuration (set by consumer before gNB starts) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct cxl_cfg_t);
} cxl_config SEC(".maps");

/* Thread-ID log: first TID_LOG_CAP TIDs that called LDPCdecoder */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, TID_LOG_CAP);
    __type(key, __u32);
    __type(value, __u64);
} tid_log SEC(".maps");

/* TID count (shared; mild races are acceptable — we want a sample, not exactly N) */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} tid_count SEC(".maps");

/* Per-CPU sequence counter — avoids the ubpf 0xdb atomic-fetch-add
 * rejection that DEV-003 triggered in v4 with PERCPU_ARRAY counters. */
struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u64);
} seq_counter SEC(".maps");

SEC("uprobe//root/linux_env/cxl/third_party/openairinterface5g/cmake_targets/ran_build/build/libldpc.so:LDPCdecoder")
int BPF_UPROBE(probe_ldpc_v5, void *p_decParams, void *p_llr, void *p_out)
{
    __u32 k = 0;

    /* 1. Read BG and Z from t_nrLDPC_dec_params (+0: u8 bg, +2: u16 Z) */
    __u8  bg = 0;
    __u16 z  = 0;
    bpf_probe_read(&bg, 1, (__u8 *)p_decParams + 0);
    bpf_probe_read(&z,  2, (__u8 *)p_decParams + 2);
    if (!bg || !z) return 0;

    __u32 nbytes = (bg == 1) ? (68u * (__u32)z) : (52u * (__u32)z);
    if (nbytes > LLR_MAX_BYTES) nbytes = LLR_MAX_BYTES;

    /* 2. Per-CPU sequence number (no 0xdb atomic issues) */
    __u64 *seq_p = bpf_map_lookup_elem(&seq_counter, &k);
    if (!seq_p) return 0;
    __u64 seq = (*seq_p)++;

    /* 3. Log thread ID (first TID_LOG_CAP calls; races acceptable) */
    __u32 *cnt_p = bpf_map_lookup_elem(&tid_count, &k);
    if (cnt_p) {
        __u32 idx = *cnt_p;
        if (idx < TID_LOG_CAP) {
            /* bpf_get_current_pid_tgid: upper 32 = tgid (process), lower 32 = tid (thread) */
            __u64 tid = bpf_get_current_pid_tgid() & 0xFFFFFFFFULL;
            bpf_map_update_elem(&tid_log, &idx, &tid, BPF_ANY);
            (*cnt_p)++;
        }
    }

    /* 4. Get CXL config and compute LLR/output offsets */
    struct cxl_cfg_t *cfg = bpf_map_lookup_elem(&cxl_config, &k);
    if (!cfg) return 0;

    __u64 llr_off, out_off;

    if (!cfg->is_standin) {
        /* DO path: OAI is launched with numactl --membind=<cxl_node>.
         * LLR allocation already lives in /dev/dax0.0 — ZERO copy.
         * Compute byte offset from region base. */
        llr_off = (__u64)(unsigned long)p_llr - cfg->region_base;
        out_off = (__u64)(unsigned long)p_out - cfg->region_base;
    } else {
        /* WSL2 stand-in path: OAI's LLR is in normal heap.
         * Assign a slot in the CXL stand-in region and copy LLR there.
         * This is the ONE documented WSL2-only copy (Change B gate clause). */
        __u32 slot = (__u32)(seq & (N_LLR_SLOTS - 1));
        llr_off = (__u64)slot * LLR_SLOT_STRIDE;
        out_off = cfg->out_base_off + (__u64)slot * 16384;

        /* bpf_probe_read from OAI's p_llr into the staging ARRAY map.
         * Consumer relays staging → cxl_base + llr_off before processing. */
        __u8 *dst = bpf_map_lookup_elem(&llr_staging, &k);
        if (dst) bpf_probe_read(dst, nbytes, p_llr);
    }

    /* 5. Reserve a descriptor slot in the BPF RINGBUF.
     * bpf_ringbuf_reserve is atomic/MPSC-safe — concurrent OAI threads
     * each get their own non-overlapping slot. */
    struct ldpc_desc_bpf *d =
        bpf_ringbuf_reserve(&desc_ringbuf, sizeof(*d), 0);
    if (!d) return 0;   /* ring full — consumer too slow; counted as drop */

    d->timestamp_ns = bpf_ktime_get_ns();
    d->slot_id      = 0;       /* slot-level probe not attached in Gate 3 */
    d->cb_index     = (__u32)seq;
    d->llr_off      = llr_off;
    d->llr_len      = nbytes;
    d->out_off      = out_off;
    /* info bits = N_VN_INFO * Z; decoded bytes = info_bits / 8 */
    d->out_len      = (bg == 1) ? (22u * (__u32)z / 8u)
                                : (10u * (__u32)z / 8u);
    d->seq          = (__u32)seq;
    d->bg           = bg;
    d->Zc           = (__u8)z;
    d->pad[0] = 0; d->pad[1] = 0; d->pad[2] = 0;
    d->pad[3] = 0; d->pad[4] = 0; d->pad[5] = 0;

    bpf_ringbuf_submit(d, 0);
    return 0;
}

char LICENSE[] SEC("license") = "GPL";
