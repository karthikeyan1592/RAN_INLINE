/* llr_mover.bpf.c — Gate 3 bpftime uprobe handler
 *
 * Fires at ldpc_decoder_impl::decode() entry.
 * Copies LLR buffer from process memory into shared BPF array map.
 * Consumer (llr_gate3.c) polls llr_slot.seq and reads the data.
 *
 * ABI (x86-64 SysV, confirmed by v6 gate2_xproc disasm + fetcharg evidence):
 *   rdi = this
 *   rsi = bit_buffer& output
 *   rdx = span<llr>.data  ← LLR pointer  (PARM3)
 *   rcx = span<llr>.size  ← element count (PARM4)
 *
 * Compile inside VM:
 *   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
 *     -I. -I/usr/include \
 *     -c llr_mover.bpf.c -o llr_mover.bpf.o
 */

#define BPF_NO_GLOBAL_DATA
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>

#define LLR_PER_CB  26112   /* N_VN_FULL(68) × Z(384) × sizeof(int8_t) */

struct llr_slot {
    __u64  seq;             /* incremented each write; consumer polls this */
    __s8   data[LLR_PER_CB];
};

/* Single-slot array: SPSC — one bpftime handler writes, one consumer reads */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct llr_slot);
} llr_map SEC(".maps");

/* Stats: how many times the handler fired */
struct stats_t {
    __u64 fires;
    __u64 bytes_written;
};
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct stats_t);
} llr_stats SEC(".maps");

SEC("uprobe/ldpc_decode")
int llr_mover(struct pt_regs *ctx)
{
    __u32 key = 0;
    struct llr_slot *slot;
    struct stats_t *st;

    /* rdx = span<llr>.data (PARM3), rcx = span<llr>.size (PARM4) */
    void *llr_ptr = (void *)PT_REGS_PARM3(ctx);
    __u64  llr_len = (__u64)PT_REGS_PARM4(ctx);

    if (!llr_ptr || llr_len == 0)
        return 0;

    __u32 copy_len = llr_len < LLR_PER_CB ? (__u32)llr_len : LLR_PER_CB;

    slot = bpf_map_lookup_elem(&llr_map, &key);
    if (!slot)
        return 0;

    if (bpf_probe_read_user(slot->data, copy_len, llr_ptr) != 0)
        return 0;

    /* SPSC: single writer — no atomic needed for data, only for seq signal */
    __sync_fetch_and_add(&slot->seq, 1);

    st = bpf_map_lookup_elem(&llr_stats, &key);
    if (st) {
        __sync_fetch_and_add(&st->fires, 1);
        __sync_fetch_and_add(&st->bytes_written, copy_len);
    }

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
