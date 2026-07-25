/* lddc_llr_mover.bpf.c — v8 bpftime uprobe handler (~50 lines of logic)
 *
 * Fires at ldpc_decoder_impl::decode() entry.
 *
 * On each fire:
 *   1. Read LLR pointer from %rdx (span<llr>.data, PARM3; confirmed from v6
 *      fetcharg: %rdx = 0x6409d141da10, LLR[0..2] = -10,+10,+10).
 *   2. Copy LLR bytes from srsRAN memory → scratch_map[seq % RING_CAP]
 *      (bpftime shared mem) via bpf_probe_read_user.
 *   3. Push a 40-byte descriptor to ring_map so the consumer knows which
 *      scratch_map slot holds this CB's LLR. The consumer reads scratch_map
 *      directly and copies to the CXL shm region itself — the handler never
 *      touches CXL memory (DEV-040: bpf_probe_write_user removed; see below).
 *   4. Increment ring_head so the consumer's busy-poll loop advances.
 *
 * config_map[0].cxl_base is set by the cxl_init.so LD_PRELOAD constructor
 * that maps the CXL shm_open region inside the benchmark process after exec,
 * then reports its VA to the consumer via /tmp/cxl_va_v8.bin. The consumer
 * updates config_map before attaching this uprobe — so cxl_base != 0 by
 * the time the first uprobe fires.
 *
 * Compile inside VM (vmlinux.h already generated in this directory):
 *   BPFTIME=/root/cxl/third_party/bpftime
 *   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
 *     -I. -I${BPFTIME}/build/libbpf \
 *     -c lddc_llr_mover.bpf.c -o lddc_llr_mover.bpf.o
 *
 * Or on GCP host (vendor headers, no vmlinux.h required):
 *   CXL=/home/karthix25/cxl
 *   clang -O2 -g -target bpf -D__TARGET_ARCH_x86 \
 *     -I${CXL}/third_party/bpftime/vendor/libbpf/include \
 *     -I${CXL}/third_party/bpftime/vendor/libbpf/include/uapi \
 *     -c lddc_llr_mover.bpf.c -o lddc_llr_mover.bpf.o
 *   (In this case replace #include "vmlinux.h" with the ifdef block below)
 */

#define BPF_NO_GLOBAL_DATA

#ifdef NO_VMLINUX  /* set -DNO_VMLINUX when compiling on GCP host without BTF */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
typedef unsigned char  __u8;
typedef unsigned short __u16;
typedef unsigned int   __u32;
typedef unsigned long long __u64;
typedef signed char    __s8;
#else
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#endif

/* ── Constants ─────────────────────────────────────────────────────────── */
#define LLR_PER_CB    26112   /* n_vn_full(68) × Z(384) = max LLR buffer */
#define BITS_PER_CB   1056    /* ceil(n_vn_info(22) × Z(384) / 8) */
#define RING_CAP      256     /* must match desc_ring.h DESC_RING_CAPACITY */
#define CB_STRIDE     (LLR_PER_CB + BITS_PER_CB + 64)  /* per-CB slot in CXL */
#define MAX_CBS       512     /* how many CB slots fit in the 64 MB region */

/* ── BPF maps ──────────────────────────────────────────────────────────── */

/* Config: consumer writes cxl_base VA before forking benchmark.
 * DEV-040: no longer used by handler (no probe_write_user); kept so
 * consumer map-fd bookkeeping doesn't break. */
struct config_t {
    __u64 cxl_base;     /* VA of CXL region in the benchmark process */
    __u32 region_size;  /* bytes (64 MB = 67108864) */
    __u32 _pad;
};
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct config_t);
} config_map SEC(".maps");

/* Scratch ring: RING_CAP slots so each decode() fires into a distinct slot.
 * DEV-040: replaced single-entry scratch_map + probe_write_user with a ring
 * of RING_CAP slots.  Consumer reads LLR directly from this map and copies
 * to CXL shm itself, avoiding bpf_probe_write_user (which installs a SIGSEGV
 * handler that conflicts with subsequent SIGSEGV in the benchmark process). */
struct scratch_t { __s8 data[LLR_PER_CB]; };
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, RING_CAP);
    __type(key, __u32);
    __type(value, struct scratch_t);
} scratch_map SEC(".maps");

/* Descriptor ring: 256 entries × 40 bytes — consumer busy-polls head counter */
struct desc_t {
    __u64 timestamp_ns;
    __u32 llr_offset;   /* byte offset into CXL region */
    __u32 llr_len;
    __u32 out_offset;   /* = llr_offset + llr_len, aligned */
    __u32 out_len;      /* = BITS_PER_CB */
    __u32 seq;
    __u32 _pad;
};  /* 40 bytes */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, RING_CAP);
    __type(key, __u32);
    __type(value, struct desc_t);
} ring_map SEC(".maps");

/* Head counter: handler increments after writing each descriptor */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} ring_head SEC(".maps");

/* DEV-041: fire counter — incremented at TOP of handler, before any early return.
 * Consumer reads this after benchmark exits to confirm handler invocation count. */
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, __u32);
} fire_count SEC(".maps");

/* ── Handler ───────────────────────────────────────────────────────────── */

SEC("uprobe/ldpc_decode")
int llr_mover(struct pt_regs *ctx)
{
    /* DEV-041: count every invocation before any early exit. */
    __u32 fc_key = 0;
    __u32 *fc = bpf_map_lookup_elem(&fire_count, &fc_key);
    if (fc) *fc = *fc + 1;

    /* %rdx = span<llr>.data (PARM3), %rcx = span<llr>.size (PARM4)
     * Confirmed from v6: gate2_xproc fetcharg %rdx=0x6409d141da10,
     * LLR[0..2]=-10,+10,+10. Confirmed from v8 debug run:
     * PARM3(RDX)=0x5674f356f8c0, PARM4(RCX)=0x4b00=19200. */
    void  *llr_ptr = (void *)PT_REGS_PARM3(ctx);
    __u64  llr_len = (__u64)PT_REGS_PARM4(ctx);
    if (!llr_ptr || llr_len == 0) return 0;

    /* Step 1: reserve a ring slot BEFORE reading, so consumer index matches
     * the scratch_map slot we write to.  Single-threaded decoder → no race. */
    __u32 key = 0;
    __u32 *head_p = bpf_map_lookup_elem(&ring_head, &key);
    if (!head_p) return 0;
    __u32 seq      = *head_p;
    __u32 ring_slot = seq & (RING_CAP - 1);

    /* Step 2: srsRAN heap → scratch_map[ring_slot] (bpftime shm).
     * DEV-040: avoid bpf_probe_write_user — it installs a SIGSEGV handler
     * that is never restored, causing subsequent SIGSEGVs from CXL page
     * faults to throw C++ exceptions from signal context → abort() → SIGILL.
     * Consumer will read from scratch_map and copy to CXL shm itself. */
    struct scratch_t *s = bpf_map_lookup_elem(&scratch_map, &ring_slot);
    if (!s) return 0;

    __u32 len = (__u32)(llr_len < LLR_PER_CB ? llr_len : LLR_PER_CB);
    if (bpf_probe_read_user(s->data, len, llr_ptr) != 0) return 0;

    /* Step 3: write descriptor — ring_slot encodes the scratch_map key */
    struct desc_t *d = bpf_map_lookup_elem(&ring_map, &ring_slot);
    if (d) {
        d->timestamp_ns = bpf_ktime_get_ns();
        d->llr_offset   = ring_slot;  /* scratch slot index (not CXL byte offset) */
        d->llr_len      = len;
        d->out_offset   = 0;          /* consumer computes CXL offset from seq */
        d->out_len      = BITS_PER_CB;
        d->seq          = seq;
        d->_pad         = 0;
    }

    /* Step 4: publish.  Single-producer non-atomic increment safe here.
     * DEV-039: __sync_fetch_and_add → opcode 0xc3 which ubpf rejects. */
    *head_p = *head_p + 1;

    return 0;
}

char LICENSE[] SEC("license") = "GPL";
