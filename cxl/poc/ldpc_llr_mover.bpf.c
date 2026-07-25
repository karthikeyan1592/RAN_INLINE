/*
 * ldpc_llr_mover.bpf.c
 *
 * Phase 3: uprobe attached to srsRAN LDPC decode() entry.
 * Intercepts the LLR input buffer pointer and copies it into the
 * shared CXL memfd region (fd stored in .rodata via skeleton).
 *
 * Attach point: ldpc_decoder_impl::decode(srsran::span<int8_t> input, ...)
 *   → offset derived inside VM: nm ldpc_decoder_benchmark | grep ldpc_decoder_impl.*decode
 *
 * Shared region layout (matches phase2_cxl_ocl_sentinel.c):
 *   [0 .. LLR_MAX_BYTES)  : LLR payload (copied from input span)
 *   [LLR_MAX_BYTES]       : uint32_t  length written
 *   [LLR_MAX_BYTES+4]     : uint32_t  sequence counter
 */

#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

/* Maximum LLR buffer size: 27 * 384 * 8 / 8 = 10368 bytes for BG1 max codeword */
#define LLR_MAX_BYTES   12288
#define REGION_SIZE     (LLR_MAX_BYTES + 16)

/* Metadata offsets within shared region */
#define META_LEN_OFF    LLR_MAX_BYTES
#define META_SEQ_OFF    (LLR_MAX_BYTES + 4)

/*
 * The shared CXL memfd region is mmap-mapped into the uprobe helper
 * process and exposed to eBPF via a per-cpu array map.
 * Each element is a fixed-size slot: [llr_bytes | len | seq]
 */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, REGION_SIZE);
} llr_region SEC(".maps");

/* Stats map: uprobe hit count, bytes copied */
struct stats_t {
    __u64 hits;
    __u64 bytes_total;
};
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __uint(key_size, sizeof(__u32));
    __uint(value_size, sizeof(struct stats_t));
} stats SEC(".maps");

/*
 * uprobe on ldpc_decoder_impl::decode()
 *
 * ABI (x86-64 System V, first arg = this ptr):
 *   rdi = this (ldpc_decoder_impl*)
 *   rsi = srsran::span<const int8_t>  { ptr, size }  — LLR input
 *   rdx = srsran::span<uint8_t>        { ptr, size }  — output bits
 *   rcx = unsigned  cb_size
 *   r8  = float     noise_var
 *   r9  = unsigned  max_iterations
 *
 * srsran::span is { T* data; size_t size } (16 bytes by value on stack
 * when passed by value, or in rsi:rdx pair by register for small spans).
 * Empirically srsRAN passes span<int8_t> input in rsi (ptr) + rdx (len).
 */
SEC("uprobe/ldpc_decode")
int BPF_KPROBE(ldpc_decode_entry)
{
    __u32 key = 0;
    void *slot;
    struct stats_t *s;

    /* rdi=this, rsi=bit_buffer&, rdx=span.data (LLR ptr), rcx=span.size */
    const void *llr_ptr = (const void *)PT_REGS_PARM3(ctx);
    __u64       llr_len = (__u64)PT_REGS_PARM4(ctx);

    if (!llr_ptr || llr_len == 0)
        return 0;

    /* Clamp to map slot capacity */
    __u32 copy_len = llr_len < LLR_MAX_BYTES ? (__u32)llr_len : LLR_MAX_BYTES;

    slot = bpf_map_lookup_elem(&llr_region, &key);
    if (!slot)
        return 0;

    /* Copy LLR bytes into the shared region */
    long rc = bpf_probe_read_user(slot, copy_len, llr_ptr);
    if (rc != 0)
        return 0;

    /* Write metadata */
    __u32 *meta_len = (__u32 *)((char *)slot + META_LEN_OFF);
    __u32 *meta_seq = (__u32 *)((char *)slot + META_SEQ_OFF);
    *meta_len = copy_len;
    __sync_fetch_and_add(meta_seq, 1);

    /* Update stats */
    s = bpf_map_lookup_elem(&stats, &key);
    if (s) {
        __sync_fetch_and_add(&s->hits, 1);
        __sync_fetch_and_add(&s->bytes_total, copy_len);
    }

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
